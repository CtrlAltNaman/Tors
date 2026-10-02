"""Replay the actual prepared OGG sample through the local backend on loopback.

Takes about 21 seconds. No ESP32 is contacted and no audio device is opened.
"""
import asyncio
import json
from pathlib import Path
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from local_backend import LocalBackend, parse_audio_frame
from local_playback import load_pcm_wav
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve


async def verify_sample():
    pcm = load_pcm_wav(ROOT / "test_audio/SampleAudio_16k_mono.wav")
    count = (len(pcm) + 639) // 640
    with tempfile.TemporaryDirectory(dir=ROOT / "tests", prefix=".tmp_sample_playback_") as directory:
        backend = LocalBackend(Path(directory), playback_pcm=pcm)
        async with serve(backend.handler, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            async with connect(f"ws://127.0.0.1:{port}/v1/stream") as client:
                await client.send(json.dumps({"type": "hello", "proto": 1,
                    "sample_rate": 16000, "codecs": ["pcm_s16le"], "device_id": "sample-loopback"}))
                assert json.loads(await client.recv())["type"] == "hello_ack"
                start = json.loads(await client.recv())
                assert start["type"] == "play_start"
                audio_id = start["audio_id"]
                received = bytearray()
                packets = acks = 0
                first_at = last_at = None
                async with asyncio.timeout(30):
                    while True:
                        message = await client.recv()
                        if isinstance(message, str):
                            control = json.loads(message)
                            if control["type"] == "play_stop":
                                assert control["audio_id"] == audio_id
                                break
                            assert control["type"] == "metrics_ack"
                            acks += 1
                            continue
                        frame = parse_audio_frame(message)
                        assert frame.stream_id == audio_id
                        assert frame.sequence == packets & 0xFFFF
                        assert frame.sample_offset == packets * 320
                        assert frame.flags == ((1 if packets == 0 else 0) | (2 if packets == count - 1 else 0))
                        received.extend(frame.payload)
                        last_at = time.perf_counter()
                        if first_at is None:
                            first_at = last_at
                        packets += 1
                        if packets % 100 == 0:
                            await client.send(json.dumps({"type": "metrics", "device_id": "sample-loopback"}))
                assert packets == count == 998
                assert received == pcm + bytes(count * 640 - len(pcm))
                assert acks == count // 100
                span = last_at - first_at
                assert (count - 1) * 0.019 <= span <= (count - 1) * 0.020 + 5, span
                await client.send(json.dumps({"type": "play_stop", "audio_id": audio_id, "reason": "complete"}))
                await client.send(json.dumps({"type": "metrics"}))
                assert json.loads(await client.recv())["type"] == "metrics_ack"
                print(f"Sample loopback: {packets} frames, {len(pcm)//2} samples, PCM byte-exact, "
                      f"{span:.3f}s first-to-last, {acks} concurrent metric acknowledgements")


if __name__ == "__main__":
    asyncio.run(verify_sample())
