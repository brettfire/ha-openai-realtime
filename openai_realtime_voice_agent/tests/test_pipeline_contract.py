import inspect
import os
import unittest
from unittest.mock import patch

from pipecat.frames.frames import (
    Frame,
    InputAudioRawFrame,
    SystemFrame,
)
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor

from app.main import Application
from app.realtime_session_router import (
    PENDING_AUDIO_MAX_FRAMES,
    RealtimeSessionRouter,
)
from app.websocket_handler import WebSocketHandler


class PassthroughProcessor(FrameProcessor):
    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await self.push_frame(frame, direction)


class FakeTransport:
    def __init__(self):
        self.input_processor = PassthroughProcessor()
        self.output_processor = PassthroughProcessor()

    def input(self):
        return self.input_processor

    def output(self):
        return self.output_processor


class FakeRecordingService:
    def __init__(self):
        self.input_requested = False
        self.output_requested = False
        self.input_recorder = PassthroughProcessor()
        self.output_recorder = PassthroughProcessor()

    def get_input_recorder(self):
        self.input_requested = True
        return self.input_recorder

    def get_output_recorder(self):
        self.output_requested = True
        return self.output_recorder


class PipelineContractTests(unittest.IsolatedAsyncioTestCase):
    async def test_build_pipeline_does_not_start_runner(self):
        handler = WebSocketHandler()

        handler.build_pipeline(
            transport=FakeTransport(),
            session_processor=PassthroughProcessor(),
            client_id="client-1",
        )

        self.assertNotIn("create_task", inspect.getsource(WebSocketHandler.build_pipeline))

    async def test_build_pipeline_uses_recording_service_when_present(self):
        recording_service = FakeRecordingService()
        handler = WebSocketHandler(audio_recording_service=recording_service)

        handler.build_pipeline(
            transport=FakeTransport(),
            session_processor=PassthroughProcessor(),
            client_id="client-1",
        )

        self.assertTrue(recording_service.input_requested)
        self.assertTrue(recording_service.output_requested)


class ApplicationInitializationContractTests(unittest.IsolatedAsyncioTestCase):
    async def test_initialize_passes_recording_service_to_websocket_handler(self):
        recording_service = object()

        with patch.dict(
            os.environ,
            {
                "OPENAI_API_KEY": "test-key",
                "ENABLE_RECORDING": "true",
                "WEBSOCKET_HOST": "127.0.0.1",
                "WEBSOCKET_PORT": "0",
            },
            clear=True,
        ):
            with patch("app.main.AudioRecordingService", return_value=recording_service):
                app = Application()
                await app.initialize()

        self.assertIs(app.audio_recording_service, recording_service)
        self.assertIs(app.websocket_handler.audio_recording_service, recording_service)


class ApplicationSessionContractTests(unittest.TestCase):
    def test_client_connect_installs_created_service_into_session_router(self):
        source = inspect.getsource(Application.run)
        connected_handler = source.split("async def on_client_connected", 1)[1]
        connected_handler = connected_handler.split("async def on_client_disconnected", 1)[0]

        self.assertIn("service = await self._ensure_openai_service", connected_handler)
        self.assertIn("await self.session_router.start_session(client_id, service)", connected_handler)


class RealtimeSessionRouterContractTests(unittest.TestCase):
    def test_router_drops_non_system_frames_without_active_session(self):
        source = inspect.getsource(RealtimeSessionRouter.process_frame)

        self.assertIn("self._active is None", source)
        self.assertIn("StartFrame, EndFrame, CancelFrame, SystemFrame", source)


def _make_audio_frame(payload: bytes = b"\x00\x01" * 480) -> InputAudioRawFrame:
    # 480 int16 samples = 20ms @ 24 kHz mono; small enough that 200 frames
    # fits comfortably in memory for the cap test.
    return InputAudioRawFrame(audio=payload, sample_rate=24000, num_channels=1)


class RouterColdStartBufferContractTests(unittest.IsolatedAsyncioTestCase):
    """Lock in the contract: pre-roll audio that arrives during the
    cold-start gap (after WS connect, before start_session) is buffered
    rather than dropped, so the satellite's pre-roll survives the gap.
    """

    async def test_input_audio_is_buffered_while_no_active_session(self):
        router = RealtimeSessionRouter()
        frame = _make_audio_frame()

        await router.process_frame(frame, FrameDirection.DOWNSTREAM)

        self.assertEqual(len(router._pending_input_audio), 1)
        buffered_frame, buffered_dir = router._pending_input_audio[0]
        self.assertIs(buffered_frame, frame)
        self.assertEqual(buffered_dir, FrameDirection.DOWNSTREAM)

    async def test_buffer_caps_and_drops_oldest_first(self):
        router = RealtimeSessionRouter()

        overflow = 5
        frames = [
            _make_audio_frame(bytes([i & 0xFF, 0]) * 8)
            for i in range(PENDING_AUDIO_MAX_FRAMES + overflow)
        ]
        for f in frames:
            await router.process_frame(f, FrameDirection.DOWNSTREAM)

        self.assertEqual(len(router._pending_input_audio), PENDING_AUDIO_MAX_FRAMES)
        self.assertEqual(router._pending_audio_dropped, overflow)
        # Oldest `overflow` frames were popped; the buffer's new head is
        # the first frame that survived.
        head_frame, _ = router._pending_input_audio[0]
        self.assertIs(head_frame, frames[overflow])

    async def test_non_audio_non_system_frames_are_still_dropped(self):
        router = RealtimeSessionRouter()

        class NotAudioOrSystem(Frame):
            pass

        await router.process_frame(NotAudioOrSystem(), FrameDirection.DOWNSTREAM)

        self.assertEqual(len(router._pending_input_audio), 0)


if __name__ == "__main__":
    unittest.main()
