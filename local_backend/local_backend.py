"""Minimal LAN backend for testing the ESP32 WebSocket audio stream.

Run from the project root:
    python local_backend.py
    python local_backend.py --play-wav test_audio/SampleAudio_16k_mono.wav
    python local_backend.py --test-tone

It acknowledges the device hello message, accepts metrics and PCM frames, and
saves each completed stream as a 16 kHz mono WAV file. Optional speaker tests
send PCM once per connection; without a playback flag this stays capture-only.
It does not start ASR.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import secrets
import struct
import time
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from local_playback import load_pcm_wav, make_test_tone, send_playback

MAGIC = 0xA5
VERSION = 1
CODEC_PCM_S16LE = 0
SAMPLE_RATE = 16_000
CHANNELS = 1
SAMPLE_WIDTH_BYTES = 2
PCM_FRAME_BYTES = 640
HEADER = struct.Struct("<BBBBHHII")

LOG = logging.getLogger("local_backend")


@dataclass(frozen=True)
class AudioFrame:
    flags: int
    sequence: int
    stream_id: int
    sample_offset: int
    payload: bytes


@dataclass
class Stream:
    stream_id: int
    device_id: str
    pcm: bytearray = field(default_factory=bytearray)
    frames: int = 0
    started_at: float = field(default_factory=time.time)


def parse_audio_frame(raw: bytes) -> AudioFrame:
    """Parse one complete 16-byte-header + 640-byte PCM firmware frame."""
    if len(raw) < HEADER.size:
        raise ValueError(f"frame too short: {len(raw)} bytes")

    magic, version, codec, flags, sequence, payload_length, stream_id, sample_offset = (
        HEADER.unpack_from(raw)
    )
    payload = raw[HEADER.size:]

    if magic != MAGIC:
        raise ValueError(f"invalid magic: 0x{magic:02x}")
    if version != VERSION:
        raise ValueError(f"unsupported protocol version: {version}")
    if codec != CODEC_PCM_S16LE:
        raise ValueError(f"unsupported codec: {codec}")
    if payload_length != len(payload):
        raise ValueError(
            f"payload length header={payload_length}, actual={len(payload)}"
        )
    if payload_length != PCM_FRAME_BYTES:
        raise ValueError(f"unexpected PCM payload length: {payload_length}")

    return AudioFrame(flags, sequence, stream_id, sample_offset, payload)


def save_wav(path: Path, pcm: bytes) -> Path:
    """Write signed 16-bit little-endian mono PCM as a WAV file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as audio:
        audio.setnchannels(CHANNELS)
        audio.setsampwidth(SAMPLE_WIDTH_BYTES)
        audio.setframerate(SAMPLE_RATE)
        audio.writeframes(pcm)
    return path


class LocalBackend:
    def __init__(
        self,
        output_dir: Path,
        inactivity_seconds: float = 1.5,
        *,
        playback_pcm: bytes | None = None,
    ) -> None:
        self.output_dir = output_dir
        self.inactivity_seconds = inactivity_seconds
        self.playback_pcm = playback_pcm
        self._save_lock = asyncio.Lock()
        self.output_dir.mkdir(parents=True, exist_ok=True)

    async def handler(self, websocket: Any) -> None:
        hello: dict[str, Any] | None = None
        streams: dict[int, Stream] = {}
        playback_task: asyncio.Task[None] | None = None
        pending_saves: set[asyncio.Task[None]] = set()
        LOG.info("client connected from %s", websocket.remote_address)

        try:
            while True:
                try:
                    message = await asyncio.wait_for(
                        websocket.recv(),
                        timeout=self.inactivity_seconds if streams else None,
                    )
                except asyncio.TimeoutError:
                    for stream_id in list(streams):
                        self.finish_stream(streams.pop(stream_id), "inactivity", pending_saves)
                    continue

                if isinstance(message, str):
                    hello = await self.handle_control(websocket, message, hello, streams, pending_saves)
                    # Keep the completed task as the once-per-connection latch.
                    # handle_control has already sent hello_ack before this runs.
                    if (
                        hello is not None
                        and self.playback_pcm is not None
                        and playback_task is None
                    ):
                        playback_task = asyncio.create_task(
                            self.send_test_playback(websocket, self.playback_pcm),
                            name="local-playback",
                        )
                else:
                    self.handle_audio(message, hello, streams)
        except Exception as error:
            LOG.info("client disconnected: %s", error)
        finally:
            if playback_task is not None:
                playback_task.cancel()
                try:
                    await playback_task
                except asyncio.CancelledError:
                    pass
            for stream_id in list(streams):
                self.finish_stream(streams.pop(stream_id), "connection_closed", pending_saves)
            if pending_saves:
                saves = asyncio.gather(*pending_saves)
                try:
                    # Cancelling to_thread does not stop the underlying disk write.
                    await asyncio.shield(saves)
                except asyncio.CancelledError:
                    await saves
                    raise

    async def send_test_playback(self, websocket: Any, pcm: bytes) -> None:
        audio_id = secrets.randbits(32)
        try:
            await send_playback(websocket, pcm, audio_id=audio_id)
        except asyncio.CancelledError:
            LOG.info("playback sender cancelled audio_id=%d", audio_id)
            raise
        except Exception:
            # Retrieve/log failures immediately, even while the receive loop lives.
            LOG.exception("playback send failed audio_id=%d", audio_id)

    async def handle_control(
        self,
        websocket: Any,
        raw: str,
        hello: dict[str, Any] | None,
        streams: dict[int, Stream],
        pending_saves: set[asyncio.Task[None]],
    ) -> dict[str, Any] | None:
        try:
            message = json.loads(raw)
        except json.JSONDecodeError:
            await websocket.send(json.dumps({"type": "error", "code": "BAD_JSON"}))
            return hello

        if not isinstance(message, dict) or not isinstance(message.get("type"), str):
            await websocket.send(json.dumps({"type": "error", "code": "BAD_CONTROL"}))
            return hello

        kind = message["type"]
        if kind == "hello":
            codecs = message.get("codecs")
            if (
                message.get("proto") != 1
                or message.get("sample_rate") != SAMPLE_RATE
                or not isinstance(codecs, list)
                or not all(isinstance(codec, str) for codec in codecs)
                or "pcm_s16le" not in codecs
            ):
                await websocket.send(json.dumps({"type": "error", "code": "BAD_HELLO"}))
                return hello
            hello = message
            await websocket.send(json.dumps({"type": "hello_ack", "proto": 1}))
            LOG.info("hello device=%s", message.get("device_id", "unknown"))
            return hello

        if kind == "start":
            if hello is None:
                await websocket.send(json.dumps({"type": "error", "code": "HELLO_REQUIRED"}))
                return hello
            stream_id = message.get("stream_id")
            if type(stream_id) is not int or stream_id in streams:
                await websocket.send(json.dumps({"type": "error", "code": "BAD_START"}))
                return hello
            streams[stream_id] = Stream(stream_id, str(hello.get("device_id", "unknown")))
            LOG.info("start stream=%d", stream_id)
            return hello

        if kind == "stop":
            stream_id = message.get("stream_id")
            if type(stream_id) is not int:
                await websocket.send(json.dumps({"type": "error", "code": "BAD_STOP"}))
                return hello
            stream = streams.pop(stream_id, None)
            if stream is not None:
                self.finish_stream(stream, str(message.get("reason", "stop")), pending_saves)
            return hello

        if kind == "metrics":
            LOG.info(
                "metrics device=%s heap=%s hits=%s false=%s score=%s",
                message.get("device_id"),
                message.get("free_heap_after_bytes"),
                message.get("keyword_hits"),
                message.get("false_activation_count"),
                message.get("last_score"),
            )
            await websocket.send(
                json.dumps(
                    {
                        "type": "metrics_ack",
                        "device_id": message.get("device_id"),
                        "server_received_ms": time.time_ns() // 1_000_000,
                    }
                )
            )
            return hello

        if kind == "play_stop":
            reason = message.get("reason", "unknown")
            log = LOG.warning if reason == "playback_error" else LOG.info
            log("device play_stop audio_id=%s reason=%s", message.get("audio_id"), reason)
            return hello

        await websocket.send(json.dumps({"type": "error", "code": "UNKNOWN_CONTROL"}))
        return hello

    def handle_audio(
        self,
        raw: bytes,
        hello: dict[str, Any] | None,
        streams: dict[int, Stream],
    ) -> None:
        try:
            frame = parse_audio_frame(raw)
        except ValueError as error:
            LOG.warning("dropped malformed audio frame: %s", error)
            return

        stream = streams.get(frame.stream_id)
        if stream is None:
            LOG.warning("audio for unknown stream=%d", frame.stream_id)
            return

        stream.pcm.extend(frame.payload)
        stream.frames += 1
        if stream.frames == 1 or stream.frames % 25 == 0:
            LOG.info(
                "audio stream=%d frames=%d seconds=%.2f bytes=%d",
                frame.stream_id,
                stream.frames,
                len(stream.pcm) / (SAMPLE_RATE * SAMPLE_WIDTH_BYTES),
                len(stream.pcm),
            )

    def finish_stream(
        self, stream: Stream, reason: str, pending_saves: set[asyncio.Task[None]],
    ) -> None:
        """Schedule a stream already detached from receive state, owned by its save."""
        async def save() -> None:
            try:
                # Preserve serialized file writes while the event loop remains free.
                async with self._save_lock:
                    await asyncio.to_thread(self.finalize, stream, reason)
            except Exception:
                LOG.exception("save failed stream=%d reason=%s", stream.stream_id, reason)

        task = asyncio.create_task(save(), name=f"save-stream-{stream.stream_id}")
        pending_saves.add(task)
        task.add_done_callback(pending_saves.discard)

    def finalize(self, stream: Stream, reason: str) -> None:
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        path = self.output_dir / f"stream-{stream.stream_id}-{timestamp}.wav"
        save_wav(path, bytes(stream.pcm))
        LOG.info(
            "saved stream=%d reason=%s frames=%d duration=%.2fs file=%s",
            stream.stream_id,
            reason,
            stream.frames,
            len(stream.pcm) / (SAMPLE_RATE * SAMPLE_WIDTH_BYTES),
            path,
        )


async def run(
    host: str, port: int, output_dir: Path, playback_pcm: bytes | None = None,
) -> None:
    try:
        from websockets.asyncio.server import serve
    except ImportError as error:
        raise SystemExit("Install dependency first: python -m pip install websockets") from error

    backend = LocalBackend(output_dir, playback_pcm=playback_pcm)
    async with serve(
        backend.handler,
        host,
        port,
        max_size=2048,
        ping_interval=20,
        ping_timeout=20,
    ):
        LOG.info("listening on ws://%s:%d/v1/stream", host, port)
        await asyncio.Future()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--output-dir", type=Path, default=Path("local_received"))
    playback = parser.add_mutually_exclusive_group()
    playback.add_argument("--play-wav", type=Path, metavar="PATH",
                          help="send one PCM16 mono 16000 Hz WAV (at most 120 seconds)")
    playback.add_argument("--test-tone", action="store_true",
                          help="send a modest two-second tone with faded edges")
    args = parser.parse_args()

    playback_pcm = None
    if args.play_wav is not None:
        try:
            playback_pcm = load_pcm_wav(args.play_wav)
        except (OSError, ValueError) as error:
            parser.error(f"cannot load playback WAV: {error}")
    elif args.test_tone:
        playback_pcm = make_test_tone()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    asyncio.run(run(args.host, args.port, args.output_dir, playback_pcm))


if __name__ == "__main__":
    main()
