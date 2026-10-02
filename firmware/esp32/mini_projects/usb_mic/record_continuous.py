"""Record the standalone ESP32 USB microphone; Ctrl+C finalizes the WAV."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
import struct
import time
import wave
import zlib

SAMPLE_RATE = 16000
PCM_BYTES = 640
HEADER = struct.Struct('<4sIHH')
PACKET_BYTES = HEADER.size + PCM_BYTES + 4


class PacketParser:
    """Resynchronize at any stream offset; validate metadata and CRC before saving."""
    def __init__(self):
        self.buffer = bytearray()
        self.bad_packets = 0
        self.discarded_bytes = 0

    def feed(self, data):
        self.buffer.extend(data)
        frames = []
        while True:
            offset = self.buffer.find(b'MIC1')
            if offset < 0:
                discard = max(0, len(self.buffer) - 3)
                self.discarded_bytes += discard
                del self.buffer[:discard]
                break
            if offset:
                self.discarded_bytes += offset
                del self.buffer[:offset]
            if len(self.buffer) < HEADER.size:
                break
            _, sequence, length, rate = HEADER.unpack_from(self.buffer)
            if length != PCM_BYTES or rate != SAMPLE_RATE:
                self.bad_packets += 1
                self.discarded_bytes += 1
                del self.buffer[0]
                continue
            if len(self.buffer) < PACKET_BYTES:
                break
            expected_crc = struct.unpack_from('<I', self.buffer, PACKET_BYTES - 4)[0]
            if zlib.crc32(self.buffer[:PACKET_BYTES - 4]) != expected_crc:
                self.bad_packets += 1
                self.discarded_bytes += 1
                del self.buffer[0]
                continue
            frames.append((sequence, bytes(self.buffer[HEADER.size:PACKET_BYTES - 4])))
            del self.buffer[:PACKET_BYTES]
        return frames


@dataclass
class RecordingStats:
    frames: int = 0
    pcm_bytes: int = 0
    missing_frames: int = 0
    resets: int = 0
    bad_packets: int = 0
    error: str = ''


def record_stream(source, output, *, seconds=None, report=print):
    """Write incrementally, finalizing the header on Ctrl+C or serial failure."""
    parser = PacketParser()
    stats = RecordingStats()
    expected = None
    started = last_audio = last_report = time.monotonic()
    with wave.open(output, 'wb') as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        try:
            while seconds is None or time.monotonic() - started < seconds:
                data = source.read(4096)
                now = time.monotonic()
                for sequence, pcm in parser.feed(data):
                    if expected is not None and sequence != expected:
                        gap = (sequence - expected) & 0xffffffff
                        if gap < 0x80000000:
                            stats.missing_frames += gap
                        else:
                            stats.resets += 1
                    expected = (sequence + 1) & 0xffffffff
                    wav.writeframesraw(pcm)
                    stats.frames += 1
                    stats.pcm_bytes += len(pcm)
                    last_audio = now
                stats.bad_packets = parser.bad_packets
                if now - last_audio >= 10:
                    raise TimeoutError('No valid microphone audio for 10 seconds; check firmware, USB port and cable.')
                if now - last_report >= 5:
                    report(f'Audio {stats.pcm_bytes / (SAMPLE_RATE * 2):.1f}s | '
                           f'frames={stats.frames} missing={stats.missing_frames} '
                           f'bad_packets={stats.bad_packets} resets={stats.resets}')
                    last_report = now
        except KeyboardInterrupt:
            pass
        except OSError as error:
            stats.error = str(error)
    return stats


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--port', default='COM41', help='Native USB serial port (default: COM41)')
    cli.add_argument('--output-dir', type=Path, default=Path(__file__).resolve().parent / 'recordings')
    cli.add_argument('--seconds', type=float, help='Optional wall-clock recording duration; default: until Ctrl+C')
    args = cli.parse_args()
    if args.seconds is not None and (args.seconds <= 0 or not args.seconds < float('inf')):
        cli.error('--seconds must be a finite positive number')
    try:
        import serial
    except ImportError:
        cli.exit(1, 'Install pyserial: python -m pip install -r requirements.txt\n')

    path = args.output_dir / f"recording_{datetime.now():%Y%m%d_%H%M%S_%f}.wav"
    port = serial.Serial(port=None, baudrate=115200, timeout=0.5)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        port.open()
        with port:
            port.reset_input_buffer()  # Framing safely recovers after an arbitrary byte boundary.
            args.output_dir.mkdir(parents=True, exist_ok=True)
            print(f'USB mic: {args.port} | 16000 Hz, mono, 16-bit')
            print(f'Recording automatically to {path}\nPress Ctrl+C to stop and save.')
            with path.open('xb') as output:
                stats = record_stream(port, output, seconds=args.seconds)
        print(f'\nSaved: {path}\nAudio: {stats.pcm_bytes / (SAMPLE_RATE * 2):.2f}s | '
              f'frames={stats.frames} missing={stats.missing_frames} '
              f'bad_packets={stats.bad_packets} resets={stats.resets}')
        if stats.error:
            print(f'Recording ended: {stats.error}')
            return 1
        return 0
    except OSError as error:
        print(f'Cannot record: {error}')
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
