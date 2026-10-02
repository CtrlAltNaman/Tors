# ESP32-S3 USB microphone mini project

Standalone **INMP441 -> ESP32-S3 -> native USB serial -> laptop WAV recorder**.

Capture and streaming start automatically at boot. No button, wake word, Wi-Fi, backend, speaker, or LED is required. This is a USB **serial audio recorder**, not a USB Audio Class microphone selectable in Windows sound settings.

The parent KWS project and its `record.py` are unchanged. Use this folder's **`record_continuous.py`**, which understands this firmware's continuous packet stream. You can launch the recorder after the ESP32 is already powered on.

## Wiring

| INMP441 | ESP32-S3 |
|---|---|
| VDD | 3.3 V |
| GND | GND |
| SCK / BCLK | GPIO 4 |
| WS / LRCLK | GPIO 5 |
| SD | GPIO 6 |
| L/R | GND (left slot) |

Use the ESP32-S3 **native USB** port for audio, currently COM41 in your setup. COM21 is the separate UART programming/logging port. Windows port numbers can change.

Disconnect power from the MAX98357A amplifier, or hold its SD pin at GND, for this microphone-only setup. The shared microphone clocks remain active and the amplifier's DIN is not driven by this firmware.

## Build and flash

In an ESP-IDF-enabled PowerShell terminal, from the parent project:

```powershell
cd mini_projects\usb_mic
idf.py build
idf.py -p COM21 flash
```

This project targets ESP32-S3 with 16 MB flash and has its own configuration, build directory and partition table. Flashing replaces the firmware running on the board, not the parent project's source files. To restore the KWS firmware later, run `idf.py -p COM21 flash` from the parent project.

The packaged files in `bin/` are:

- `USBMic.bin`: application, offset `0x10000`.
- `bootloader.bin`: bootloader, offset `0x0`.
- `partition-table.bin`: matching partition table, offset `0x8000`.

Prefer `idf.py flash`, which writes all matching files. With only the packaged binaries and an installed esptool, from this mini-project directory:

```powershell
python -m esptool --chip esp32s3 --port COM21 --baud 460800 write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 bin/bootloader.bin 0x8000 bin/partition-table.bin 0x10000 bin/USBMic.bin
```

## Record on the laptop

Connect the native USB port with a data-capable cable, then run from this folder:

```powershell
python -m pip install -r requirements.txt
python record_continuous.py --port COM41
```

Recording starts automatically. Speak, then press **Ctrl+C once** to stop the laptop recording and finalize the WAV. The ESP32 keeps streaming and can be recorded again by rerunning the script. Files go into this folder's `recordings/` directory by default.

Optional timed recording or another output folder:

```powershell
python record_continuous.py --port COM41 --seconds 30
python record_continuous.py --port COM41 --output-dir C:\Users\91805\Desktop\MicSamples
```

The recorder writes incrementally to disk, not an ever-growing RAM buffer. A detected USB error or 10 seconds without valid packets finalizes the received audio and reports an error. Close other programs using COM41; do not run `idf.py monitor` on the audio port. Force-killing Python, disk failure or laptop power loss is not equivalent to Ctrl+C and can leave an incomplete WAV header.

## Audio and packet format

- 16,000 samples/second, mono, signed 16-bit little-endian PCM.
- 32-bit I2S **left-slot** capture; the upper 16 bits form each PCM sample, matching the previous recording conversion. No gain, denoising, normalization or model processing.
- 320 samples / 640 PCM bytes per packet (20 ms), about 50 packets/second.
- Each USB packet is 656 bytes: `MIC1` (4), sequence (uint32 LE), payload length (uint16 LE = 640), sample rate (uint16 LE = 16000), PCM payload (640), CRC32 (uint32 LE over the preceding 652 bytes).
- Standard CRC32 matches Python `zlib.crc32`. Framing is removed before audio reaches the WAV. It allows late joining, resets and resynchronization after incomplete USB reads. Audio bytes that happen to spell START/STOP are never interpreted as control messages.
- The 115200 baud setting is a USB serial API setting, not a UART link carrying 32 KB/s. This transport requires native USB, not COM21/UART.

## Diagnostics and limitations

The verified ESP-IDF build produces a 199,376-byte application with 14,552 bytes of static `.data + .bss`. This static figure excludes runtime heap, task stacks, DMA allocations and IRAM code; use the UART heap diagnostics for runtime measurements.

Optional firmware log monitor on the **separate UART port**:

```powershell
idf.py -p COM21 monitor
```

Every five seconds: captured frames, frames queued to USB, frames dropped because USB is busy, I2S overflows/read errors, internal heap usage and stack headroom. Firmware text logging is disabled on the USB audio console.

Capture uses eight I2S DMA descriptors (160 ms capacity) and a 4 KB USB TX buffer. USB writes never wait for the host. When no recorder is consuming audio, drops are expected: the ESP32 does not retain an offline recording. After connecting, steady recording should show increasing frames with no new sequence gaps or CRC failures. Queued frames are not proof that the laptop saved them.

The laptop reports `missing`, `bad_packets` and `resets`. Missing/corrupt frames are omitted and reported, not padded; a recording with drops is shorter than elapsed wall time. Device DMA overflows are reported on UART and cannot all be inferred from USB sequence numbers. There is no on-device storage and no networking/local-IP log page in this mini project.

## Tests

```powershell
python tests/test_recorder.py
gcc -std=c11 -Wall -Wextra -Werror -I main tests/protocol_test.c main/mic_protocol.c -o tests/protocol_test.exe
python tests/check_c_encoder.py
```

Host tests cover fragmented/coalesced packets, mid-stream attachment, CRC rejection, bounded garbage recovery, marker-like audio bytes, sequence gaps/wrap/reset, Ctrl+C and disconnect WAV finalization. Build and host checks do not replace listening to an actual hardware recording and checking for drops.
