# Tors

Low-latency, edge-first voice activation for the custom keyword Hello Tors.

Tors runs keyword spotting locally on an ESP32-S3 and only starts sending audio
to a backend after a local detection. The repository is organized so firmware,
model work, transport, and evaluation evidence remain separate and reproducible.

Status: active prototype and hardware-validation project. The current
implementation contains useful measurements and self-tests, but it must not yet
be presented as proven to meet the SIH limits of less than 256 KB RAM, less than
10% idle CPU, near-zero false activations, or a specific latency target. Those
claims require controlled measurement on target hardware.

## Architecture

    INMP441 microphone
            |
            v
    ESP32-S3: I2S -> PCM16 -> MFCC -> int8 TFLite Micro KWS
            |                         |
            | local keyword hit       | idle: no upload
            v                         v
    rolling prebuffer + live PCM -> WebSocket -> backend / ASR gateway
                                                       |
                                                       v
                                                 WAV / ASR / metrics

## Repository map

| Path | Responsibility |
| --- | --- |
| firmware/esp32/ | Current ESP-IDF firmware, device tests, model integration, and local backend harness |
| docs/datasheets/ | Hardware datasheets used by the project |
| presentation/ | SIH slides, diagrams, and submission assets |
| ml/ | Model-training and export boundary; dataset files stay out of Git |
| backend/ | Production ASR gateway and protocol tests |
| simulator/ | Reserved host-side replay and end-to-end simulation boundary |
| tools/ | Reserved measurement, flashing, and reproducibility tools |

The existing firmware project is intentionally kept internally coherent during
this first migration. Its internal documentation and tests continue to use
paths relative to firmware/esp32.

## Quick start

Run the dependency-light host checks:

    cd firmware/esp32
    python tests/test_local_backend.py
    python tests/test_kws_firmware_integration.py

Run the LAN capture backend:

    cd firmware/esp32
    python local_backend.py

Build the device firmware from an ESP-IDF-enabled terminal:

    cd firmware/esp32
    idf.py set-target esp32s3
    idf.py build

The device/backend contract is documented in
firmware/esp32/docs/ESP32_BACKEND_DEVICE_CONTRACT.md.

## Audio and transport contract

- 16 kHz, mono, signed 16-bit little-endian PCM
- 20 ms transport frames, 320 samples, 640 payload bytes
- 16-byte little-endian application header
- WebSocket stays connected while the device listens
- local KWS detection sends a prebuffer followed by live audio
- idle microphone audio is not continuously uploaded

## Project documentation

Project-specific documentation is kept with the firmware under
firmware/esp32/docs/. Start with the device/backend contract and the ML
deployment review there.

Hardware references are in docs/datasheets/.

## Data and secrets

Raw recordings, generated datasets, captured streams, firmware binaries, and
machine-local network credentials are ignored by Git. Commit dataset manifests,
model metadata, reproducible scripts, and benchmark reports instead.

Do not place Wi-Fi passwords, backend tokens, or private recordings in this
repository.
