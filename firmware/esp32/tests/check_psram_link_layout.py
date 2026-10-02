"""Check actual ESP32-S3 linked placement, not just source annotations."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
nm = (sys.argv[1] if len(sys.argv) > 1 else
      r'C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin\xtensa-esp32s3-elf-nm.exe')
output = subprocess.check_output(
    [nm, '-S', '--format=posix', str(root / 'build/EdgeAIKWS.elf')], text=True)
symbols = {}
for line in output.splitlines():
    fields = line.split()
    if len(fields) >= 3:
        symbols[fields[0]] = (int(fields[2], 16),
                              int(fields[3], 16) if len(fields) > 3 else 0)

ext_start = symbols['_ext_ram_bss_start'][0]
ext_end = symbols['_ext_ram_bss_end'][0]
bss_start = symbols['_bss_start'][0]
bss_end = symbols['_bss_end'][0]
data_start = symbols['_data_start'][0]
data_end = symbols['_data_end'][0]
assert ext_end > ext_start, 'missing external BSS'

for name, size in [('audio_history', 38408), ('frame_samples', 640), ('audio_frame', 656),
                   ('playback_storage', 41728), ('wire', 656)]:
    address, actual_size = symbols[name]
    assert actual_size == size, (name, actual_size)
    assert ext_start <= address < address + size <= ext_end, (name, hex(address))
    print(f'{name}: {size} B in PSRAM @ {address:#x}')

for name in ['_ZL12tensor_arena', 'i2s_buffer', 'audio_tx_buffer', 'history_epoch', 'audio_lock']:
    address, size = symbols[name]
    assert bss_start <= address < address + size <= bss_end or \
        data_start <= address < address + size <= data_end, (name, hex(address))
    print(f'{name}: {size} B internal @ {address:#x}')
assert symbols['_ZL12tensor_arena'][1] == 114688
print(f'Internal data+BSS: {data_end - data_start + bss_end - bss_start} B')
print(f'External BSS: {ext_end - ext_start} B')
