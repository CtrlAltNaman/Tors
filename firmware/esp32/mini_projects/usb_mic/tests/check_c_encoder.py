"""Run the compiled C encoder tests and check its output with the real Python parser."""
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from record_continuous import PacketParser

raw = bytes.fromhex(subprocess.check_output([str(ROOT / 'tests' / 'protocol_test.exe')], text=True).strip())
frames = PacketParser().feed(raw)
assert len(frames) == 1 and frames[0][0] == 0x12345678
assert struct.unpack('<4h', frames[0][1][:8]) == (-32768, 32767, -1, 0)
assert frames[0][1][8:] == bytes(632)
print('C encoder -> Python decoder: CRC, packet layout and signed PCM passed')
