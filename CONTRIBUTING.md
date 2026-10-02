# Contributing to Tors

## Development rules

1. Keep edge runtime code independent from training code.
2. Treat the device/backend protocol as a versioned public interface.
3. Add a host test for every new protocol or decision-rule behavior.
4. Do not commit credentials, private recordings, generated binaries, or large datasets.
5. Record hardware measurements with board, firmware commit, model hash, and test conditions.
6. Do not describe SIH limits as satisfied without reproducible evidence from the target device.

## Local verification

    python local_backend/tests/test_local_backend.py
    python local_backend/tests/test_local_playback.py
    python firmware/esp32/tests/test_kws_firmware_integration.py

If ESP-IDF is available, also run idf.py build from firmware/esp32.

## Commit guidance

Prefer focused commits such as:

- docs: define device and backend contract
- test: cover malformed audio frames
- firmware: add bounded prebuffer
- benchmark: record idle cpu on esp32-s3

Keep presentation edits separate from firmware and model changes.
