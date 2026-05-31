import inspect
import os
import unittest
from unittest.mock import patch

from pipecat.frames.frames import Frame
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor

from app.main import Application
from app.realtime_session_router import RealtimeSessionRouter
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


if __name__ == "__main__":
    unittest.main()
