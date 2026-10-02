"""Bounded PCM test sources and paced ESP32 playback; no ASR or codec dependency."""

from __future__ import annotations

import asyncio
import json
import logging
import math
import secrets
import struct
import time
from collections.abc import Awaitable, Callable, Iterator
from pathlib import Path
from typing import Any

SAMPLE_RATE = 16_000
SAMPLE_WIDTH = 2
FRAME_SAMPLES = 320
FRAME_BYTES = FRAME_SAMPLES * SAMPLE_WIDTH
FRAME_SECONDS = FRAME_SAMPLES / SAMPLE_RATE
MAX_PCM_BYTES = SAMPLE_RATE * SAMPLE_WIDTH * 120
MAX_WAV_BYTES = MAX_PCM_BYTES + 1024 * 1024  # Allow bounded RIFF metadata.
HEADER = struct.Struct("<BBBBHHII")
LOG = logging.getLogger("local_backend.playback")


def load_pcm_wav(path: Path) -> bytes:
    """Read a conventional RIFF PCM16 mono 16 kHz WAV, capped at 120 seconds.

    Validate chunk boundaries too: wave readers can silently return short data
    for truncated files. Unknown metadata chunks are allowed, but never sent.
    """
    with Path(path).open("rb") as source:
        raw = source.read(MAX_WAV_BYTES + 1)
    if len(raw) > MAX_WAV_BYTES:
        raise ValueError(f"WAV exceeds the {MAX_WAV_BYTES}-byte file limit")
    if len(raw) < 12 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError("expected a RIFF PCM WAV file")
    if struct.unpack_from("<I", raw, 4)[0] + 8 != len(raw):
        raise ValueError("WAV RIFF length mismatch (truncated or trailing data)")

    offset = 12
    have_format = False
    pcm: bytes | None = None
    while offset < len(raw):
        if offset + 8 > len(raw):
            raise ValueError("truncated WAV chunk header")
        kind, size = struct.unpack_from("<4sI", raw, offset)
        start = offset + 8
        end = start + size
        offset = end + (size & 1)
        if offset > len(raw):
            raise ValueError("truncated WAV chunk data or padding")
        if kind == b"fmt ":
            if have_format or size < 16:
                raise ValueError("invalid or duplicate WAV format chunk")
            audio_format = struct.unpack_from("<HHIIHH", raw, start)
            if audio_format != (1, 1, SAMPLE_RATE, SAMPLE_RATE * SAMPLE_WIDTH, SAMPLE_WIDTH, 16):
                raise ValueError("WAV must be uncompressed PCM16, mono, 16000 Hz")
            have_format = True
        elif kind == b"data":
            if not have_format or pcm is not None:
                raise ValueError("WAV requires one data chunk after its format chunk")
            if size == 0 or size % SAMPLE_WIDTH:
                raise ValueError("WAV must contain nonempty, complete PCM16 samples")
            if size > MAX_PCM_BYTES:
                raise ValueError("WAV duration exceeds 120 seconds")
            pcm = raw[start:end]
    if pcm is None:
        raise ValueError("WAV has no PCM data")
    return pcm


def make_test_tone() -> bytes:
    """Two seconds of 440 Hz at 12% peak, with 50 ms raised-cosine fades."""
    count = SAMPLE_RATE * 2
    fade_samples = SAMPLE_RATE // 20
    pcm = bytearray(count * SAMPLE_WIDTH)
    for index in range(count):
        edge = min(index, count - 1 - index, fade_samples) / fade_samples
        envelope = (1 - math.cos(math.pi * edge)) / 2
        sample = round(32767 * 0.12 * envelope * math.sin(2 * math.pi * 440 * index / SAMPLE_RATE))
        struct.pack_into("<h", pcm, index * SAMPLE_WIDTH, sample)
    return bytes(pcm)


def iter_pcm_packets(pcm: bytes, audio_id: int) -> Iterator[bytes]:
    """Yield complete binary messages; the last PCM payload is zero-padded."""
    if not pcm or len(pcm) % SAMPLE_WIDTH:
        raise ValueError("playback requires nonempty, complete PCM16 samples")
    if type(audio_id) is not int or not 0 <= audio_id <= 0xFFFFFFFF:
        raise ValueError("audio_id must be a u32")
    for sequence, offset in enumerate(range(0, len(pcm), FRAME_BYTES)):
        flags = (1 if offset == 0 else 0) | (2 if offset + FRAME_BYTES >= len(pcm) else 0)
        payload = pcm[offset:offset + FRAME_BYTES].ljust(FRAME_BYTES, b"\x00")
        yield HEADER.pack(
            0xA5, 1, 0, flags, sequence & 0xFFFF, FRAME_BYTES,
            audio_id, offset // SAMPLE_WIDTH,
        ) + payload


async def send_playback(
    websocket: Any,
    pcm: bytes,
    *,
    audio_id: int | None = None,
    grace_seconds: float = 1.0,
    clock: Callable[[], float] = time.perf_counter,
    sleep: Callable[[float], Awaitable[None]] = asyncio.sleep,
) -> None:
    """Send once, leaving transport errors and cancellation to the owner.

    Use the high-resolution monotonic performance clock (also on Windows 3.12).
    Deadlines include send time. If a send or scheduler stall misses the next
    deadline, resume one whole frame later rather than bursting overdue packets.
    """
    if audio_id is None:
        audio_id = secrets.randbits(32)
    packets = iter_pcm_packets(pcm, audio_id)
    first_packet = next(packets)  # Validate before sending any control message.
    await sleep(grace_seconds)
    await websocket.send(json.dumps({
        "type": "play_start", "audio_id": audio_id, "codec": "pcm_s16le",
        "sample_rate": SAMPLE_RATE, "channels": 1, "frame_ms": 20,
    }))
    deadline = clock()
    packet = first_packet
    while True:
        await sleep(max(0.0, deadline - clock()))
        await websocket.send(packet)
        packet = next(packets, None)
        if packet is None:
            break
        deadline += FRAME_SECONDS
        now = clock()
        if deadline <= now:
            deadline = now + FRAME_SECONDS
    await websocket.send(json.dumps({"type": "play_stop", "audio_id": audio_id}))
    LOG.info(
        "playback sent audio_id=%d frames=%d duration=%.2fs; awaiting device play_stop",
        audio_id, (len(pcm) + FRAME_BYTES - 1) // FRAME_BYTES,
        len(pcm) / (SAMPLE_RATE * SAMPLE_WIDTH),
    )
