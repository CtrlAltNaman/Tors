# Standalone USB microphone mini project

Scope: implement the user's automatic, button-free microphone recorder separately; leave the parent KWS project, configuration and record.py untouched. No Wi-Fi, backend, inference, speaker or USB Audio Class integration.

## Design

- INMP441, existing pins: BCLK 4, WS 5, SD 6, L/R grounded. 16 kHz mono signed 16-bit PCM, derived from 32-bit left-slot I2S samples.
- Firmware starts capture/USB transmission on boot. A bounded USB TX buffer and nonblocking writes keep unplugged/slow hosts from stalling microphone capture. Dropped frames are counted, not stored indefinitely.
- Each 20 ms packet has a sync marker, sequence, payload length, sample rate and CRC32. This lets the laptop join mid-stream and reject corrupted/incomplete frames without waiting for START/STOP. PCM itself is uncompressed.
- A separate laptop script streams validated PCM to disk, finalizes a WAV on Ctrl+C/disconnection, and supports an optional duration. It does not keep the whole recording in RAM or reinterpret audio bytes as text commands.
- ESP-IDF logs remain UART-only; no log server. Firmware emits a compact five-second capture/USB/heap summary. Python reports saved duration, received frames and gaps. Missing frames are reported and omitted, not concealed.

## Implementation and verification

- [x] Add native encoder tests and Python parser/recorder tests; run before implementation.
- [x] Implement portable CRC/frame encoder and robust bounded parser.
- [x] Implement standalone ESP-IDF configuration and microphone-only app.
- [x] Implement laptop CLI, requirements and documented wiring/build/record steps.
- [x] Verify CRC compatibility, fragmented/late-join/corrupt stream recovery, Ctrl+C and disconnected recording finalization; build firmware and inspect size.
- [x] Package the app/bootloader/partition binaries, verify original project hashes unchanged. Do not flash hardware automatically.

No Git repository is present. Hardware capture/audio-quality verification is a separate test after flashing.

Verification: seven Python tests pass, native C CRC/PCM assertions pass, C encoder output passes the Python decoder, and ESP-IDF build succeeds. Application binary: 199376 bytes (`0x30ad0`). Static `.data + .bss`: 14552 bytes; runtime heap usage is reported on UART. Parent `record.py`, `main/main.c`, `main/CMakeLists.txt`, and `sdkconfig` SHA-256 hashes are unchanged. All three packaged binaries match their build outputs. No hardware flashing or actual microphone recording was performed.
