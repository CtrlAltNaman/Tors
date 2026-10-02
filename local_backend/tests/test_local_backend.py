import struct
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from local_backend import parse_audio_frame, save_wav  # noqa: E402


def test_parse_audio_frame_matches_firmware_header():
    payload = b"\x01\x02" * 320
    raw = struct.pack("<BBBBHHII", 0xA5, 1, 0, 0x05, 7, len(payload), 42, 320)
    frame = parse_audio_frame(raw + payload)

    assert frame.stream_id == 42
    assert frame.sample_offset == 320
    assert frame.flags == 0x05
    assert frame.payload == payload


def test_parse_audio_frame_rejects_invalid_payload_length():
    raw = struct.pack("<BBBBHHII", 0xA5, 1, 0, 0, 0, 640, 1, 0)

    try:
        parse_audio_frame(raw + b"short")
    except ValueError as error:
        assert "payload length" in str(error)
    else:
        raise AssertionError("invalid frame was accepted")


def test_save_wav_writes_16khz_mono_pcm(tmp_path):
    path = save_wav(tmp_path / "sample.wav", b"\x00\x01" * 320)

    with wave.open(str(path), "rb") as audio:
        assert audio.getnchannels() == 1
        assert audio.getsampwidth() == 2
        assert audio.getframerate() == 16000
        assert audio.getnframes() == 320


if __name__ == "__main__":
    test_parse_audio_frame_matches_firmware_header()
    test_parse_audio_frame_rejects_invalid_payload_length()
    test_save_wav_writes_16khz_mono_pcm(Path("tests/.tmp_local_backend"))
    print("local backend tests passed")
