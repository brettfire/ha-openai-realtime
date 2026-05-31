"""Serializer for raw binary PCM audio + JSON control messages."""
import json
import logging
from typing import Optional, Union

from pipecat.frames.frames import (
    Frame,
    InputAudioRawFrame,
    InterruptionFrame,
    OutputAudioRawFrame,
)
from pipecat.serializers.base_serializer import FrameSerializer

logger = logging.getLogger(__name__)


class RawAudioSerializer(FrameSerializer):
    """Serializer that treats binary messages as raw PCM audio and recognizes
    a small JSON control protocol on text frames.

    Control messages currently supported:
      - ``{"type": "interrupt"}`` — emit an ``InterruptionFrame`` so the
        downstream ``OpenAIRealtimeLLMService`` cancels the in-flight bot
        response. Used by the ESP32 satellite for wake-word barge-in
        without dropping/rebuilding the WebSocket session.

    Unknown text frames are dropped with a debug log.
    """

    async def deserialize(
        self, message: Union[bytes, str]
    ) -> Optional[Frame]:
        """Deserialize an incoming WebSocket message into a pipecat Frame.

        Binary messages → ``InputAudioRawFrame`` (24 kHz, 16-bit, mono).
        Text messages → parsed as JSON; recognized control types emit
        the appropriate pipecat frame. Returns ``None`` for anything we
        don't know how to handle, which the transport drops silently.
        """
        # Text frames: try to parse as our JSON control protocol
        if isinstance(message, str):
            try:
                data = json.loads(message)
            except (json.JSONDecodeError, TypeError):
                logger.debug(
                    "📨 Ignoring non-JSON text frame (%d chars)", len(message)
                )
                return None
            msg_type = data.get("type") if isinstance(data, dict) else None
            if msg_type == "interrupt":
                logger.info(
                    "🛑 Interrupt control message received — emitting "
                    "InterruptionFrame to cancel bot response"
                )
                return InterruptionFrame()
            logger.debug(
                "📨 Ignoring unknown JSON control message type=%r", msg_type
            )
            return None

        if not isinstance(message, bytes):
            return None

        # Validate audio format: 16-bit = 2 bytes per sample
        if len(message) % 2 != 0:
            logger.warning(
                "⚠️ Received audio with odd byte count: %d bytes, skipping",
                len(message),
            )
            return None

        # Audio is 24kHz, 16-bit, mono PCM
        return InputAudioRawFrame(
            audio=message,
            sample_rate=24000,
            num_channels=1,
        )
    
    async def serialize(self, frame: Frame) -> str | bytes | None:
        """Serialize frame to binary message.
        
        For output audio frames, return raw audio bytes. For interruption
        frames, send a text control message so the satellite can stop local
        playback and clear buffered audio.
        """
        if isinstance(frame, OutputAudioRawFrame):
            audio_bytes = frame.audio
            logger.debug(f"📤 Serializing OutputAudioRawFrame: {len(audio_bytes)} bytes")
            return audio_bytes
        if isinstance(frame, InterruptionFrame):
            logger.info("📤 Serializing InterruptionFrame control message")
            return json.dumps({"type": "interrupt"})
        # For other frame types, return None (not serialized)
        logger.debug(f"📤 Serializing non-audio frame: {type(frame).__name__}, returning None")
        return None
