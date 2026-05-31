"""Dynamic per-client OpenAI Realtime session routing."""
import logging
from collections import deque
from dataclasses import dataclass
from typing import Deque, Optional, Tuple

from pipecat.frames.frames import (
    CancelFrame,
    EndFrame,
    Frame,
    InputAudioRawFrame,
    StartFrame,
    SystemFrame,
)
from pipecat.pipeline.task import FrameProcessorSetup
from pipecat.processors.aggregators.llm_response_universal import LLMContextAggregatorPair
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor
from pipecat.services.openai.realtime.llm import OpenAIRealtimeLLMService

from app.session_manager import SessionManager, _shutdown_service_safely

logger = logging.getLogger(__name__)

# Upper bound on the cold-start input-audio queue. Each pipecat
# InputAudioRawFrame from our satellite is ~100ms of 24 kHz mono
# PCM (~4.8 KB), so 200 frames is ~20 seconds — comfortably larger
# than the worst observed MCP+OpenAI cold-start window, small
# enough that a stuck session-create can't blow heap.
PENDING_AUDIO_MAX_FRAMES = 200


class StartFrameFilter(FrameProcessor):
    """Drops the active session's replayed StartFrame at the outer pipeline edge."""

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)

        if isinstance(frame, StartFrame):
            return

        await self.push_frame(frame, direction)


@dataclass
class ActiveRealtimeSession:
    client_id: str
    service: OpenAIRealtimeLLMService
    context_aggregator: Optional[LLMContextAggregatorPair]
    processors: list[FrameProcessor]


class RealtimeSessionRouter(FrameProcessor):
    """Installs one OpenAI Realtime session into a running websocket pipeline.

    Pipecat's websocket server is started by the outer pipeline before a client
    exists. This router keeps that server alive while allowing every websocket
    connection to get its own OpenAIRealtimeLLMService and context aggregator.
    """

    def __init__(self, session_manager: Optional[SessionManager] = None, **kwargs):
        super().__init__(**kwargs)
        self.session_manager = session_manager
        self._setup: Optional[FrameProcessorSetup] = None
        self._start_frame: Optional[StartFrame] = None
        self._downstream: Optional[FrameProcessor] = None
        self._active: Optional[ActiveRealtimeSession] = None
        # Holds InputAudioRawFrames that arrived while _active is None
        # (i.e. during _ensure_openai_service cold start). Replayed
        # in order at start_session so the satellite's pre-roll isn't
        # silently discarded. Bounded to PENDING_AUDIO_MAX_FRAMES.
        self._pending_input_audio: Deque[Tuple[InputAudioRawFrame, FrameDirection]] = deque()
        self._pending_audio_dropped: int = 0

    @property
    def active_service(self) -> Optional[OpenAIRealtimeLLMService]:
        return self._active.service if self._active else None

    @property
    def active_client_id(self) -> Optional[str]:
        return self._active.client_id if self._active else None

    async def setup(self, setup: FrameProcessorSetup):
        await super().setup(setup)
        self._setup = setup

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)

        if isinstance(frame, StartFrame):
            self._start_frame = frame

        # Buffer input audio that arrives during the cold-start window
        # (after WS connect, before start_session installs a service).
        # Without this, the satellite ships ~1500ms of pre-roll audio
        # the moment the WS opens and the entire pre-roll is
        # silently misrouted out the transport.output before the
        # OpenAI service is even in the pipeline. Replayed in
        # start_session in original order.
        #
        # IMPORTANT: InputAudioRawFrame is itself a SystemFrame in
        # pipecat 0.0.97, so this branch MUST run before the
        # system-frame allowlist below — otherwise the allowlist
        # would let audio frames flow through unbuffered.
        if self._active is None and isinstance(frame, InputAudioRawFrame):
            if len(self._pending_input_audio) >= PENDING_AUDIO_MAX_FRAMES:
                # Cap reached — drop oldest so we retain the most
                # recent audio (a never-ending cold start shouldn't
                # leak memory; the newer frames are also more useful
                # for whatever just happened).
                self._pending_input_audio.popleft()
                self._pending_audio_dropped += 1
            self._pending_input_audio.append((frame, direction))
            return

        if self._active is None and not isinstance(
            frame, (StartFrame, EndFrame, CancelFrame, SystemFrame)
        ):
            logger.debug("Dropping %s while no realtime session is active", type(frame).__name__)
            return

        await self.push_frame(frame, direction)

    async def start_session(
        self,
        client_id: str,
        service: OpenAIRealtimeLLMService,
    ) -> None:
        """Install a new per-client service into the running pipeline."""

        if self._setup is None or self._start_frame is None:
            raise RuntimeError("RealtimeSessionRouter has not received pipeline setup/start")

        if self._active:
            await self.end_session(cache_context=True)

        logger.info("🔗 Installing OpenAI Realtime session for client %s", client_id)

        context_aggregator = (
            self.session_manager.create_context_aggregator(client_id)
            if self.session_manager
            else None
        )
        tail = StartFrameFilter()

        if context_aggregator:
            processors: list[FrameProcessor] = [
                context_aggregator.user(),
                service,
                context_aggregator.assistant(),
                tail,
            ]
        else:
            processors = [service, tail]

        for processor in processors:
            await processor.setup(self._setup)

        for previous, current in zip(processors, processors[1:]):
            previous.link(current)

        self._downstream = self._next
        if self._downstream is not None:
            tail.link(self._downstream)

        self.link(processors[0])
        self._active = ActiveRealtimeSession(
            client_id=client_id,
            service=service,
            context_aggregator=context_aggregator,
            processors=processors,
        )

        await processors[0].queue_frame(self._start_frame)

        # Replay any input audio captured during the cold-start gap.
        # Order matters — pipecat InputAudioRawFrames are stitched
        # into a continuous PCM stream by the downstream OpenAI
        # service, so reordering would garble what the user said.
        if self._pending_input_audio:
            pending = list(self._pending_input_audio)
            self._pending_input_audio.clear()
            dropped = self._pending_audio_dropped
            self._pending_audio_dropped = 0
            total_bytes = sum(len(frame.audio) for frame, _ in pending)
            duration_ms = (total_bytes // 2) * 1000 // 24000  # 16-bit @ 24 kHz mono
            if dropped:
                logger.warning(
                    "📦 Replaying %d buffered input frames (~%dms) into new session "
                    "for client %s — %d earlier frames were dropped "
                    "(cap=%d) before this point",
                    len(pending), duration_ms, client_id, dropped, PENDING_AUDIO_MAX_FRAMES,
                )
            else:
                logger.info(
                    "📦 Replaying %d buffered input frames (~%dms) into new session for client %s",
                    len(pending), duration_ms, client_id,
                )
            for frame, frame_direction in pending:
                await processors[0].queue_frame(frame, frame_direction)

    async def end_session(self, cache_context: bool = True) -> None:
        """Remove and shut down the active per-client service."""

        if not self._active:
            return

        active = self._active
        self._active = None

        # Drop any pre-roll buffered after this session ended but
        # before a new one starts — that audio belongs to no
        # session and replaying it into the *next* client's session
        # would be wrong (different speaker, different intent).
        if self._pending_input_audio:
            logger.debug(
                "Discarding %d buffered input frames at end_session for client %s",
                len(self._pending_input_audio), active.client_id,
            )
            self._pending_input_audio.clear()
            self._pending_audio_dropped = 0

        if self._downstream is not None:
            self.link(self._downstream)

        if cache_context and self.session_manager:
            self.session_manager.handle_client_disconnect(active.client_id, active.service)
        else:
            await _shutdown_service_safely(active.service)

        for processor in active.processors:
            if processor is active.service:
                continue
            try:
                await processor.cleanup()
            except Exception as e:
                logger.debug("Error cleaning up session processor %s: %s", processor, e)

        logger.info("🔌 Removed OpenAI Realtime session for client %s", active.client_id)

    async def cleanup(self):
        await self.end_session(cache_context=False)
        await super().cleanup()
