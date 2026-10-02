"""Host-only playback tests: deterministic clocks and ephemeral loopback sockets."""

import asyncio
import importlib.util
import inspect
import io
import json
import math
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import wave
from contextlib import asynccontextmanager
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import local_backend
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve

playback = None
if importlib.util.find_spec("local_playback") is not None:
    import local_playback as playback

HEADER = struct.Struct("<BBBBHHII")
HELLO = {"type": "hello", "proto": 1, "sample_rate": 16000,
         "codecs": ["pcm_s16le"], "device_id": "loopback-test"}


def wav_bytes(pcm=b"\x01\x02" * 320, *, channels=1, rate=16000, width=2):
    target = io.BytesIO()
    with wave.open(target, "wb") as audio:
        audio.setnchannels(channels)
        audio.setsampwidth(width)
        audio.setframerate(rate)
        audio.writeframes(pcm)
    return target.getvalue()


class PlaybackCase(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(playback, "local_playback implementation is missing")


class WavTests(PlaybackCase):
    def load(self, raw):
        with tempfile.TemporaryDirectory(dir=ROOT / "tests", prefix=".tmp_playback_") as directory:
            path = Path(directory) / "input.wav"
            path.write_bytes(raw)
            return playback.load_pcm_wav(path)

    def test_load_returns_only_pcm_and_accepts_metadata(self):
        pcm = b"\x01\x02" * 321
        raw = wav_bytes(pcm)
        metadata = b"JUNK" + struct.pack("<I", 3) + b"abc\x00"
        raw = raw[:36] + metadata + raw[36:]
        raw = raw[:4] + struct.pack("<I", len(raw) - 8) + raw[8:]
        self.assertEqual(self.load(raw), pcm)

    def test_rejects_incompatible_formats(self):
        compressed = bytearray(wav_bytes())
        struct.pack_into("<H", compressed, 20, 3)  # IEEE float, not PCM.
        wrong_alignment = bytearray(wav_bytes())
        struct.pack_into("<H", wrong_alignment, 32, 4)
        wrong_byte_rate = bytearray(wav_bytes())
        struct.pack_into("<I", wrong_byte_rate, 28, 64000)
        for raw in (wav_bytes(channels=2), wav_bytes(rate=8000),
                    wav_bytes(width=1), wav_bytes(width=3), bytes(compressed),
                    bytes(wrong_alignment), bytes(wrong_byte_rate), b"OggSnot-wave"):
            with self.subTest(raw=raw[:36]):
                with self.assertRaises(ValueError):
                    self.load(raw)

    def test_rejects_empty_and_truncated_containers(self):
        valid = wav_bytes()
        truncated_data = bytearray(valid[:-2])
        struct.pack_into("<I", truncated_data, 4, len(truncated_data) - 8)
        for raw in (b"", wav_bytes(b""), valid[:30], valid[:-1],
                    bytes(truncated_data), wav_bytes(b"\x00\x01\x02")):
            with self.subTest(length=len(raw)):
                with self.assertRaises(ValueError):
                    self.load(raw)

    def test_duration_is_bounded_at_120_seconds(self):
        pcm = bytes(16000 * 2 * 120)
        self.assertEqual(len(self.load(wav_bytes(pcm))), len(pcm))
        with self.assertRaises(ValueError):
            self.load(wav_bytes(pcm + b"\x00\x00"))

    def test_file_size_is_bounded_even_with_metadata(self):
        raw = wav_bytes() + b"JUNK" + struct.pack("<I", 8_000_000) + bytes(8_000_000)
        raw = raw[:4] + struct.pack("<I", len(raw) - 8) + raw[8:]
        with self.assertRaises(ValueError):
            self.load(raw)

    def test_tone_is_two_seconds_modest_and_fades_both_edges(self):
        pcm = playback.make_test_tone()
        self.assertEqual(len(pcm), 16000 * 2 * 2)
        samples = struct.unpack("<32000h", pcm)
        self.assertEqual((samples[0], samples[-1]), (0, 0))
        self.assertGreater(max(samples), 1000)
        self.assertLessEqual(max(abs(value) for value in samples), 5000)
        rms = lambda values: math.sqrt(sum(value * value for value in values) / len(values))
        self.assertLess(rms(samples[:160]), rms(samples[16000:16160]) / 3)
        self.assertLess(rms(samples[-160:]), rms(samples[16000:16160]) / 3)
        self.assertLess(max(abs(b - a) for a, b in zip(samples, samples[1:])), 1500)


class PacketTests(PlaybackCase):
    def test_headers_flags_offsets_and_zero_padding(self):
        pcm = b"\x01\x02" * 641
        packets = list(playback.iter_pcm_packets(pcm, 0xFEDCBA98))
        self.assertEqual(len(packets), 3)
        for sequence, packet in enumerate(packets):
            self.assertIsInstance(packet, bytes)
            self.assertEqual(len(packet), 656)
            self.assertEqual(HEADER.unpack_from(packet),
                             (0xA5, 1, 0, [1, 0, 2][sequence], sequence,
                              640, 0xFEDCBA98, sequence * 320))
        self.assertEqual(b"".join(packet[16:] for packet in packets),
                         pcm + bytes(640 - 2))

    def test_single_frame_sets_both_flags_without_extra_packet(self):
        for pcm in (b"\x01\x02", b"\x01\x02" * 320):
            packets = list(playback.iter_pcm_packets(pcm, 1))
            self.assertEqual(len(packets), 1)
            self.assertEqual(HEADER.unpack_from(packets[0])[3], 3)

    def test_sequence_wrap_does_not_wrap_sample_offset(self):
        for index, packet in enumerate(playback.iter_pcm_packets(bytes(640 * 65537), 1)):
            if index >= 65535:
                header = HEADER.unpack_from(packet)
                self.assertEqual(header[4], index & 65535)
                self.assertEqual(header[7], index * 320)

    def test_rejects_empty_odd_pcm_and_invalid_audio_ids(self):
        for pcm, audio_id in ((b"", 1), (b"x", 1), (b"xx", -1),
                              (b"xx", 2**32), (b"xx", True)):
            with self.subTest(pcm=pcm, audio_id=audio_id):
                with self.assertRaises(ValueError):
                    list(playback.iter_pcm_packets(pcm, audio_id))


class FakeClock:
    def __init__(self, oversleep_call=None):
        self.now = 50.0
        self.positive_sleeps = 0
        self.oversleep_call = oversleep_call

    def time(self):
        return self.now

    async def sleep(self, delay):
        if delay < 0:
            raise AssertionError("negative sleep")
        self.now += delay
        if delay > 0:
            self.positive_sleeps += 1
            if self.positive_sleeps == self.oversleep_call:
                self.now += 0.150


class Transport:
    def __init__(self, clock, delays=()):
        self.clock = clock
        self.delays = iter(delays)
        self.messages = []
        self.binary_times = []
        self.binary_ends = []

    async def send(self, message):
        self.messages.append(message)
        if isinstance(message, bytes):
            self.binary_times.append(self.clock.time())
            self.clock.now += next(self.delays, 0.0)
            self.binary_ends.append(self.clock.time())


class SenderTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.assertIsNotNone(playback, "local_playback implementation is missing")

    async def send(self, clock, transport, *, grace=0):
        await playback.send_playback(transport, b"\x01\x02" * 1281,
                                     audio_id=42, grace_seconds=grace,
                                     clock=clock.time, sleep=clock.sleep)

    def test_default_clock_is_high_resolution_and_monotonic(self):
        clock = inspect.signature(playback.send_playback).parameters["clock"].default
        self.assertIs(clock, time.perf_counter)
        self.assertTrue(time.get_clock_info("perf_counter").monotonic)
        self.assertLess(time.get_clock_info("perf_counter").resolution, 0.001)

    async def test_real_sender_controls_packets_and_grace(self):
        clock = FakeClock()
        transport = Transport(clock)
        with self.assertLogs("local_backend", level="INFO") as logs:
            await self.send(clock, transport, grace=1.0)
        self.assertEqual(json.loads(transport.messages[0]),
                         {"type": "play_start", "audio_id": 42, "codec": "pcm_s16le",
                          "sample_rate": 16000, "channels": 1, "frame_ms": 20})
        self.assertEqual(json.loads(transport.messages[-1]),
                         {"type": "play_stop", "audio_id": 42})
        packets = transport.messages[1:-1]
        self.assertEqual(packets, list(playback.iter_pcm_packets(b"\x01\x02" * 1281, 42)))
        self.assertAlmostEqual(transport.binary_times[0], 51.0)
        self.assertIn("playback sent", " ".join(logs.output))
        self.assertNotIn("reason=complete", " ".join(logs.output))

    async def test_normal_send_time_is_included_in_20ms_deadlines(self):
        clock = FakeClock()
        transport = Transport(clock, [0.006] * 5)
        await self.send(clock, transport)
        for index, instant in enumerate(transport.binary_times):
            self.assertAlmostEqual(instant, 50 + index * 0.020)

    async def test_slow_send_rebases_without_catch_up_burst(self):
        clock = FakeClock()
        transport = Transport(clock, [0.006, 0.150, 0.006, 0.006, 0.006])
        await self.send(clock, transport)
        self.assertGreaterEqual(transport.binary_times[2] - transport.binary_ends[1], 0.0199)
        for first, second in zip(transport.binary_times, transport.binary_times[1:]):
            self.assertGreaterEqual(second - first, 0.0199)

    async def test_scheduler_stall_rebases_without_catch_up_burst(self):
        clock = FakeClock(oversleep_call=1)
        transport = Transport(clock)
        await self.send(clock, transport)
        self.assertGreater(transport.binary_times[1] - transport.binary_times[0], 0.1)
        for first, second in zip(transport.binary_times, transport.binary_times[1:]):
            self.assertGreaterEqual(second - first, 0.0199)

    async def test_transport_error_reaches_caller(self):
        class BrokenTransport:
            async def send(self, message):
                raise OSError("test transport failure")

        clock = FakeClock()
        with self.assertRaisesRegex(OSError, "test transport failure"):
            await self.send(clock, BrokenTransport())


class CliTests(unittest.TestCase):
    def cli(self, *args):
        return subprocess.run([sys.executable, str(ROOT / "local_backend.py"), *args],
                              capture_output=True, text=True, timeout=5, cwd=ROOT)

    def test_help_documents_both_modes(self):
        result = self.cli("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("--play-wav", result.stdout)
        self.assertIn("--test-tone", result.stdout)

    def test_modes_are_mutually_exclusive(self):
        result = self.cli("--play-wav", "unused.wav", "--test-tone")
        self.assertEqual(result.returncode, 2)
        self.assertIn("not allowed with argument", result.stderr)

    def test_invalid_wav_fails_before_serving(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "tests", prefix=".tmp_playback_") as directory:
            path = Path(directory) / "invalid.wav"
            path.write_bytes(b"OggSnot-a-wav")
            result = self.cli("--play-wav", str(path), "--host", "127.0.0.1", "--port", "0")
        self.assertEqual(result.returncode, 2)
        self.assertIn("WAV", result.stderr)
        self.assertNotIn("Traceback", result.stderr)


class LifecycleTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=ROOT / "tests", prefix=".tmp_playback_")
        self.addCleanup(self.directory.cleanup)
        self.output_dir = Path(self.directory.name)
        self.loop_errors = []
        loop = asyncio.get_running_loop()
        previous = loop.get_exception_handler()
        loop.set_exception_handler(lambda _loop, context: self.loop_errors.append(context))
        self.addCleanup(loop.set_exception_handler, previous)

    async def asyncTearDown(self):
        self.assertEqual(self.loop_errors, [], "unhandled background task errors")

    def backend(self, pcm):
        self.assertIsNotNone(playback, "local_playback implementation is missing")
        return local_backend.LocalBackend(self.output_dir, playback_pcm=pcm)

    @asynccontextmanager
    async def server(self, backend, *, fail_binary=False):
        self.handler_done = asyncio.Event()

        async def handler(socket):
            if fail_binary:
                original_send = socket.send

                async def send(message):
                    if isinstance(message, bytes):
                        raise OSError("injected binary send failure")
                    await original_send(message)

                socket.send = send
            try:
                await backend.handler(socket)
            finally:
                self.handler_done.set()

        async with serve(handler, "127.0.0.1", 0, ping_interval=None) as server:
            port = server.sockets[0].getsockname()[1]
            yield f"ws://127.0.0.1:{port}/v1/stream"

    async def receive(self, client):
        return await asyncio.wait_for(client.recv(), 3.0)

    async def hello(self, client):
        await client.send(json.dumps(HELLO))
        self.assertEqual(json.loads(await self.receive(client)), {"type": "hello_ack", "proto": 1})

    async def test_default_connection_remains_capture_only(self):
        backend = local_backend.LocalBackend(self.output_dir)
        async with self.server(backend) as uri, connect(uri) as client:
            await self.hello(client)
            with self.assertRaises(asyncio.TimeoutError):
                await asyncio.wait_for(client.recv(), 1.1)
            await client.send(json.dumps({"type": "metrics", "device_id": "test"}))
            self.assertEqual(json.loads(await self.receive(client))["type"], "metrics_ack")

    async def test_invalid_hello_does_not_start_sender(self):
        backend = self.backend(b"\x01\x02" * 320)
        async with self.server(backend) as uri, connect(uri) as client:
            await client.send(json.dumps({**HELLO, "sample_rate": 8000}))
            self.assertEqual(json.loads(await self.receive(client))["code"], "BAD_HELLO")
            with self.assertRaises(asyncio.TimeoutError):
                await asyncio.wait_for(client.recv(), 1.1)
            await self.hello(client)
            self.assertEqual(json.loads(await self.receive(client))["type"], "play_start")

    async def test_hello_codecs_requires_list_of_strings_and_exact_codec(self):
        from websockets.exceptions import ConnectionClosed

        backend = local_backend.LocalBackend(self.output_dir)
        async with self.server(backend) as uri:
            for codecs in (None, "not_pcm_s16le", "pcm_s16le", {"pcm_s16le": 1},
                           ["pcm_s16le", None], ["not_pcm_s16le"]):
                with self.subTest(codecs=codecs):
                    async with connect(uri) as client:
                        await client.send(json.dumps({**HELLO, "codecs": codecs}))
                        try:
                            response = await self.receive(client)
                        except ConnectionClosed:
                            self.fail("malformed hello disconnected the client")
                        self.assertEqual(json.loads(response), {"type": "error", "code": "BAD_HELLO"})
                        await self.hello(client)
                        await client.send(json.dumps({"type": "metrics"}))
                        self.assertEqual(json.loads(await self.receive(client))["type"], "metrics_ack")

    async def test_unhashable_stream_ids_return_errors_without_disconnect(self):
        from websockets.exceptions import ConnectionClosed

        backend = local_backend.LocalBackend(self.output_dir)
        async with self.server(backend) as uri:
            for kind in ("start", "stop"):
                for stream_id in ([], {}):
                    with self.subTest(kind=kind, stream_id=stream_id):
                        async with connect(uri) as client:
                            await self.hello(client)
                            await client.send(json.dumps({"type": "start", "stream_id": 1}))
                            await client.send(json.dumps({"type": kind, "stream_id": stream_id}))
                            try:
                                response = await self.receive(client)
                            except ConnectionClosed:
                                self.fail("malformed stream_id disconnected the client")
                            self.assertEqual(json.loads(response),
                                             {"type": "error", "code": "BAD_" + kind.upper()})
                            await client.send(json.dumps({"type": "metrics"}))
                            self.assertEqual(json.loads(await self.receive(client))["type"], "metrics_ack")

    async def test_slow_saves_allow_playback_and_metrics_and_are_drained(self):
        for reason in ("stop", "inactivity"):
            with self.subTest(reason=reason):
                backend = local_backend.LocalBackend(self.output_dir, inactivity_seconds=0.03,
                                                     playback_pcm=bytes(640 * 100))
                entered = asyncio.Event()
                release = threading.Event()
                finished = threading.Event()
                loop = asyncio.get_running_loop()
                original_save = local_backend.save_wav

                def slow_save(path, pcm):
                    loop.call_soon_threadsafe(entered.set)
                    try:
                        release.wait(2.0)  # Bound the deliberate stall even on the old blocking code.
                        return original_save(path, pcm)
                    finally:
                        finished.set()

                with patch("local_backend.save_wav", side_effect=slow_save):
                    async with self.server(backend) as uri:
                        try:
                            async with connect(uri) as client:
                                await self.hello(client)
                                self.assertEqual(json.loads(await self.receive(client))["type"], "play_start")
                                self.assertIsInstance(await self.receive(client), bytes)
                                stream_id = 21 if reason == "stop" else 22
                                await client.send(json.dumps({"type": "start", "stream_id": stream_id}))
                                payload = b"\x11\x22" * 320
                                await client.send(HEADER.pack(0xA5, 1, 0, 3, 0, 640, stream_id, 0) + payload)
                                if reason == "stop":
                                    await client.send(json.dumps({"type": "stop", "stream_id": stream_id}))
                                await asyncio.wait_for(entered.wait(), 3.0)
                                self.assertFalse(finished.is_set(), "WAV write blocked the event loop")
                                await client.send(json.dumps({"type": "metrics"}))
                                packets = 0
                                metrics = False
                                while packets < 3 or not metrics:
                                    response = await self.receive(client)
                                    if isinstance(response, bytes):
                                        packets += 1
                                    else:
                                        self.assertEqual(json.loads(response)["type"], "metrics_ack")
                                        metrics = True
                                self.assertFalse(finished.is_set(), "save must still be pending during playback/receive")
                                # A repeated stop must not finalize this detached stream a second time.
                                await client.send(json.dumps({"type": "stop", "stream_id": stream_id}))
                            with self.assertRaises(asyncio.TimeoutError):
                                await asyncio.wait_for(self.handler_done.wait(), 0.05)
                        finally:
                            release.set()
                            await asyncio.wait_for(self.handler_done.wait(), 3.0)
                self.assertTrue(finished.is_set())
                recordings = list(self.output_dir.glob(f"stream-{stream_id}-*.wav"))
                self.assertEqual(len(recordings), 1)
                with wave.open(str(recordings[0]), "rb") as recorded:
                    self.assertEqual(recorded.readframes(640), payload)

    async def test_disconnect_saves_open_stream_in_worker_and_waits_for_it(self):
        backend = local_backend.LocalBackend(self.output_dir)
        entered = asyncio.Event()
        release = threading.Event()
        finished = threading.Event()
        loop = asyncio.get_running_loop()
        original_save = local_backend.save_wav

        def slow_save(path, pcm):
            loop.call_soon_threadsafe(entered.set)
            try:
                release.wait(2.0)
                return original_save(path, pcm)
            finally:
                finished.set()

        with patch("local_backend.save_wav", side_effect=slow_save):
            async with self.server(backend) as uri:
                try:
                    async with connect(uri) as client:
                        await self.hello(client)
                        await client.send(json.dumps({"type": "start", "stream_id": 23}))
                        await client.send(HEADER.pack(0xA5, 1, 0, 3, 0, 640, 23, 0) + bytes(640))
                    await asyncio.wait_for(entered.wait(), 3.0)
                    self.assertFalse(finished.is_set(), "disconnect WAV write blocked the event loop")
                    self.assertFalse(self.handler_done.is_set(), "connection cleanup must drain saves")
                finally:
                    release.set()
                    await asyncio.wait_for(self.handler_done.wait(), 3.0)
        self.assertTrue(finished.is_set())
        with wave.open(str(next(self.output_dir.glob("*.wav"))), "rb") as recorded:
            self.assertEqual(recorded.readframes(320), bytes(640))

    async def test_save_failure_is_logged_without_stopping_receive(self):
        backend = local_backend.LocalBackend(self.output_dir)
        with self.assertLogs("local_backend", level="ERROR") as logs:
            with patch("local_backend.save_wav", side_effect=OSError("injected disk failure")):
                async with self.server(backend) as uri:
                    async with connect(uri) as client:
                        await self.hello(client)
                        await client.send(json.dumps({"type": "start", "stream_id": 24}))
                        await client.send(HEADER.pack(0xA5, 1, 0, 3, 0, 640, 24, 0) + bytes(640))
                        await client.send(json.dumps({"type": "stop", "stream_id": 24}))
                        await client.send(json.dumps({"type": "metrics"}))
                        try:
                            response = await self.receive(client)
                        except Exception as error:
                            self.fail(f"save failure interrupted receive: {error}")
                        self.assertEqual(json.loads(response)["type"], "metrics_ack")
                    await asyncio.wait_for(self.handler_done.wait(), 3.0)
        self.assertIn("injected disk failure", " ".join(logs.output))
        self.assertIn("save failed stream=24", " ".join(logs.output))

    async def test_full_duplex_once_per_connection_and_device_completion(self):
        backend = self.backend(b"\x01\x02" * (320 * 20 + 1))
        with self.assertLogs("local_backend", level="INFO") as logs:
            async with self.server(backend) as uri, connect(uri) as client:
                started = asyncio.get_running_loop().time()
                await self.hello(client)
                await self.hello(client)  # A duplicate during grace must not schedule another clip.
                start = json.loads(await self.receive(client))
                self.assertEqual(start["type"], "play_start")
                self.assertGreaterEqual(asyncio.get_running_loop().time() - started, 0.90)
                audio_id = start["audio_id"]
                self.assertIs(type(audio_id), int)
                self.assertTrue(0 <= audio_id <= 0xFFFFFFFF)
                self.assertEqual(len(await self.receive(client)), 656)
                await client.send(json.dumps({"type": "start", "stream_id": 17}))
                upload = HEADER.pack(0xA5, 1, 0, 3, 0, 640, 17, 0) + b"\x11\x22" * 320
                await client.send(upload)
                await client.send(json.dumps({"type": "stop", "stream_id": 17, "reason": "test"}))
                await client.send(json.dumps({"type": "metrics", "device_id": "test"}))
                packets = 1
                received_metrics = False
                while True:
                    message = await self.receive(client)
                    if isinstance(message, bytes):
                        packets += 1
                        continue
                    control = json.loads(message)
                    if control["type"] == "play_stop":
                        self.assertEqual(control["audio_id"], audio_id)
                        break
                    self.assertEqual(control["type"], "metrics_ack")
                    received_metrics = True
                self.assertTrue(received_metrics, "metrics must be handled during playback")
                self.assertEqual(packets, 21)
                recordings = list(self.output_dir.glob("*.wav"))
                self.assertEqual(len(recordings), 1)
                with wave.open(str(recordings[0]), "rb") as recorded:
                    self.assertEqual(recorded.readframes(320), b"\x11\x22" * 320)
                for reason in ("complete", "playback_error"):
                    await client.send(json.dumps({"type": "play_stop", "audio_id": audio_id, "reason": reason}))
                    await client.send(json.dumps({"type": "metrics"}))
                    self.assertEqual(json.loads(await self.receive(client))["type"], "metrics_ack")
                await self.hello(client)  # No replay after the sender has finished, either.
                with self.assertRaises(asyncio.TimeoutError):
                    await asyncio.wait_for(client.recv(), 1.1)
        output = " ".join(logs.output)
        self.assertIn("playback sent", output)
        self.assertIn("device play_stop", output)
        self.assertIn("reason=complete", output)
        self.assertIn("reason=playback_error", output)
        self.assertNotIn("UNKNOWN_CONTROL", output)

    async def assert_disconnect_cleanup(self, *, during_playback):
        backend = self.backend(bytes(32000 * 2))
        baseline = asyncio.all_tasks()
        async with self.server(backend) as uri:
            async with connect(uri) as client:
                await self.hello(client)
                if during_playback:
                    self.assertEqual(json.loads(await self.receive(client))["type"], "play_start")
                    self.assertIsInstance(await self.receive(client), bytes)
            await asyncio.wait_for(self.handler_done.wait(), 0.5)
        # Both socket and handler have closed: no task may outlive this connection.
        remaining = {task for task in asyncio.all_tasks() - baseline if not task.done()}
        self.assertEqual(remaining, set())

    async def test_disconnect_cancels_and_awaits_sender_during_grace(self):
        await self.assert_disconnect_cleanup(during_playback=False)

    async def test_disconnect_cancels_and_awaits_active_sender(self):
        await self.assert_disconnect_cleanup(during_playback=True)

    async def test_sender_failure_is_logged_while_receive_loop_keeps_working(self):
        backend = self.backend(bytes(640 * 5))
        with self.assertLogs("local_backend", level="ERROR") as logs:
            async with self.server(backend, fail_binary=True) as uri, connect(uri) as client:
                await self.hello(client)
                self.assertEqual(json.loads(await self.receive(client))["type"], "play_start")
                await client.send(json.dumps({"type": "metrics"}))
                self.assertEqual(json.loads(await self.receive(client))["type"], "metrics_ack")
                self.assertTrue(any("injected binary send failure" in log for log in logs.output))
        self.assertIn("playback send failed", " ".join(logs.output))


if __name__ == "__main__":
    unittest.main(verbosity=2)
