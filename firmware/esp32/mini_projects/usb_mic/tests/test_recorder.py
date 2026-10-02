import io
from pathlib import Path
import struct
import sys
import unittest
import wave
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from record_continuous import PacketParser, record_stream


def packet(sequence, pcm=None):
    pcm = pcm if pcm is not None else bytes(range(256)) * 2 + bytes(range(128))
    content = struct.pack('<4sIHH', b'MIC1', sequence, 640, 16000) + pcm
    return content + struct.pack('<I', zlib.crc32(content))


class RecorderTests(unittest.TestCase):
    def test_fragmented_packet_and_coalesced_packets(self):
        parser = PacketParser()
        raw = packet(7)
        frames = []
        for byte in raw:
            frames.extend(parser.feed(bytes([byte])))
        self.assertEqual(frames, [(7, raw[12:-4])])
        self.assertEqual([seq for seq, _ in parser.feed(packet(8) + packet(9))], [8, 9])

    def test_join_midstream_and_resync_after_corruption(self):
        parser = PacketParser()
        bad = bytearray(packet(1))
        bad[100] ^= 0x80
        data = b'boot log\n' + packet(0)[90:] + bad + packet(2)
        self.assertEqual([seq for seq, _ in parser.feed(data)], [2])
        self.assertEqual(parser.bad_packets, 1)

    def test_invalid_header_and_bounded_garbage(self):
        parser = PacketParser()
        malformed = struct.pack('<4sIHH', b'MIC1', 0, 65535, 16000)
        self.assertEqual(parser.feed(malformed + b'x' * 20000), [])
        self.assertLessEqual(len(parser.buffer), 3)
        self.assertEqual([seq for seq, _ in parser.feed(packet(3))], [3])

    def test_audio_containing_text_markers_is_not_a_control_message(self):
        parser = PacketParser()
        pcm = (b'START STOP MIC1' * 64)[:640]
        self.assertEqual(parser.feed(packet(4, pcm)), [(4, pcm)])

    def test_ctrl_c_finalizes_wav_and_counts_sequence_gaps(self):
        class Source:
            count = 0
            def read(self, size):
                self.count += 1
                if self.count == 1:
                    return packet(10) + packet(12)
                raise KeyboardInterrupt
        output = io.BytesIO()
        stats = record_stream(Source(), output, report=lambda _: None)
        self.assertEqual((stats.frames, stats.missing_frames, stats.pcm_bytes), (2, 1, 1280))
        output.seek(0)
        with wave.open(output, 'rb') as wav:
            self.assertEqual((wav.getframerate(), wav.getnchannels(), wav.getsampwidth(), wav.getnframes()), (16000, 1, 2, 640))

    def test_disconnect_saves_partial_recording_and_reports_error(self):
        class Source:
            count = 0
            def read(self, size):
                self.count += 1
                if self.count == 1:
                    return packet(0)
                raise OSError('USB disconnected')
        output = io.BytesIO()
        stats = record_stream(Source(), output, report=lambda _: None)
        self.assertIn('USB disconnected', stats.error)
        output.seek(0)
        with wave.open(output, 'rb') as wav:
            self.assertEqual(wav.getnframes(), 320)

    def test_sequence_wrap_and_reset_do_not_count_billions_of_gaps(self):
        class Source:
            count = 0
            def read(self, size):
                self.count += 1
                if self.count == 1:
                    return packet(0xffffffff) + packet(0) + packet(5) + packet(0)
                raise KeyboardInterrupt
        stats = record_stream(Source(), io.BytesIO(), report=lambda _: None)
        self.assertEqual(stats.missing_frames, 4)
        self.assertEqual(stats.resets, 1)


if __name__ == '__main__':
    unittest.main()
