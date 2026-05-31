import json
import unittest

from pipecat.frames.frames import InputAudioRawFrame, InterruptionFrame, OutputAudioRawFrame

from app.raw_audio_serializer import RawAudioSerializer


class RawAudioSerializerContractTests(unittest.IsolatedAsyncioTestCase):
    async def test_binary_pcm_deserializes_to_24khz_mono_input_audio(self):
        frame = await RawAudioSerializer().deserialize(b"\x01\x00\x02\x00")

        self.assertIsInstance(frame, InputAudioRawFrame)
        self.assertEqual(frame.audio, b"\x01\x00\x02\x00")
        self.assertEqual(frame.sample_rate, 24000)
        self.assertEqual(frame.num_channels, 1)

    async def test_interrupt_text_deserializes_to_interruption_frame(self):
        frame = await RawAudioSerializer().deserialize('{"type":"interrupt"}')

        self.assertIsInstance(frame, InterruptionFrame)

    async def test_output_audio_serializes_as_raw_pcm_bytes(self):
        frame = OutputAudioRawFrame(
            audio=b"\x01\x00\x02\x00",
            sample_rate=24000,
            num_channels=1,
        )

        payload = await RawAudioSerializer().serialize(frame)

        self.assertEqual(payload, b"\x01\x00\x02\x00")

    async def test_interruption_frame_serializes_as_satellite_text_control(self):
        payload = await RawAudioSerializer().serialize(InterruptionFrame())

        self.assertIsInstance(payload, str)
        self.assertEqual(json.loads(payload), {"type": "interrupt"})


if __name__ == "__main__":
    unittest.main()
