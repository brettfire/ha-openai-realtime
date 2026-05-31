import inspect
import os
import unittest
from unittest.mock import patch

from pipecat.frames.frames import Frame
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor

from app.main import Application
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

        with patch("app.websocket_handler.asyncio.create_task") as create_task:
            handler.build_pipeline(
                transport=FakeTransport(),
                openai_service=PassthroughProcessor(),
                client_id="client-1",
            )

        create_task.assert_not_called()

    async def test_build_pipeline_uses_recording_service_when_present(self):
        recording_service = FakeRecordingService()
        handler = WebSocketHandler(audio_recording_service=recording_service)

        handler.build_pipeline(
            transport=FakeTransport(),
            openai_service=PassthroughProcessor(),
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


class KnownContractGapTests(unittest.TestCase):
    @unittest.expectedFailure
    def test_client_connect_service_is_inserted_into_the_pipeline(self):
        """Known gap: client connect creates a new service outside the pipeline."""

        source = inspect.getsource(Application.run)
        connected_handler = source.split("async def on_client_connected", 1)[1]
        connected_handler = connected_handler.split("def on_client_disconnected", 1)[0]

        self.assertIn("_build_pipeline_for_transport", connected_handler)


if __name__ == "__main__":
    unittest.main()
