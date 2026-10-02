import asyncio
import json

import pytest
import websockets
from websockets.asyncio.server import serve

from backend.protocol import AudioFrame, FLAG_FIRST, FLAG_LAST, unpack
from backend.server import VoiceServer


@pytest.mark.asyncio
async def test_metrics_are_accepted_and_acknowledged(tmp_path):
    server = VoiceServer(tmp_path)
    metrics = {
        "type": "metrics",
        "device_id": "metrics-device",
        "model": "hello_tors_int8",
        "uptime_ms": 1234,
        "model_flash_bytes": 36744,
        "inference_us_avg": 1200,
        "keyword_hits": 2,
    }

    async with serve(server.handler, "127.0.0.1", 0) as listening:
        port = listening.sockets[0].getsockname()[1]
        async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
            await ws.send(json.dumps({
                "type": "hello",
                "proto": 1,
                "device_id": "metrics-device",
                "codecs": ["pcm_s16le"],
                "sample_rate": 16000,
            }))
            assert json.loads(await ws.recv()) == {"type": "hello_ack", "proto": 1}

            await ws.send(json.dumps(metrics))
            assert json.loads(await ws.recv())["type"] == "metrics_ack"

    assert server.latest_metrics["metrics-device"]["keyword_hits"] == 2


@pytest.mark.asyncio
async def test_server_can_send_playback_frames(tmp_path):
    server = VoiceServer(tmp_path)
    received = []

    async def device_handler(ws):
        await ws.send(json.dumps({
            "type": "hello",
            "proto": 1,
            "device_id": "speaker-device",
            "codecs": ["pcm_s16le"],
            "sample_rate": 16000,
        }))
        await ws.recv()
        await ws.send(json.dumps({
            "type": "play_start",
            "audio_id": 7,
            "codec": "pcm_s16le",
            "sample_rate": 16000,
            "channels": 1,
            "frame_ms": 20,
        }))
        received.append(json.loads(await ws.recv()))
        received.append(unpack(await ws.recv()))
        received.append(json.loads(await ws.recv()))

    async with serve(device_handler, "127.0.0.1", 0) as listening:
        port = listening.sockets[0].getsockname()[1]
        async with websockets.connect(f"ws://127.0.0.1:{port}") as device:
            await device.recv()
            await device.send(json.dumps({"type": "hello_ack", "proto": 1}))
            await asyncio.sleep(0)
            await server.send_playback(
                device,
                audio_id=7,
                pcm=b"\x01\x00" * 321,
            )

    assert received[0]["type"] == "play_start"
    assert received[1].flags == FLAG_FIRST | FLAG_LAST
    assert received[1].stream_id == 7
    assert len(received[1].payload) == 640
    assert received[2] == {"type": "play_stop", "audio_id": 7, "reason": "complete"}
