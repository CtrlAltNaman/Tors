# EdgeAIKWS Project Report

**Project:** TORS — Low-Latency Voice Activation for Edge Devices  
**SIH Problem Statement:** 26172 — Low Latency and Efficient Voice Activator for Edge Devices  
**Organization:** Indian Space Research Organisation (ISRO)  
**Device keyword:** “Hello Tors”  
**Report date:** 29 September 2026

## 1. Executive summary

EdgeAIKWS is a custom-keyword voice-activation prototype. An ESP32-S3 listens to an INMP441 microphone and runs an INT8 TensorFlow Lite Micro keyword model locally. When the device accepts “Hello Tors,” it can stream buffered and live microphone audio to a Python WebSocket backend for speech recognition. This keeps idle microphone audio on-device rather than continuously uploading it.

The repository contains the trained model artifacts, an ESP-IDF firmware integration, a versioned PCM WebSocket protocol, a Python ASR backend, and an ASR-free local playback test server. Host-side tests and build checks cover important components. Physical-device detection reliability, competition-level RAM/idle-CPU compliance, end-to-end latency percentiles, and usable MAX98357A playback are not yet established; these are called out below rather than presented as achieved results.

## 2. Problem and intended behavior

The SIH challenge calls for local detection of a team-trained keyword, followed by efficient audio transfer to a remote ASR service. The design separates the two jobs:

- **ESP32-S3:** continuously captures audio and performs local KWS.
- **Python backend:** receives a session only after KWS activation, reconstructs audio, and runs server-side ASR.

The custom phrase is “Hello Tors”; the firmware does not use a generic assistant wake-word model. While listening, it sends connection control and telemetry, but does not stream ambient microphone PCM unless KWS triggers.

## 3. System architecture

```text
INMP441 microphone
  → ESP32-S3 I2S capture (16 kHz)
  → PCM16 frames + rolling audio history
  ├─→ required model MFCC frontend → INT8 TFLite Micro → temporal decision
  │                                                   └─ accepted keyword
  │                                                      → red trigger indication
  │                                                      → local WAV recording
  │                                                      → WebSocket audio session
  └───────────────────────────────────────────────────────┘
                                                        ↓
                                    Python WebSocket backend
                                      → frame validation / WAV
                                      → Vosk ASR worker / logs

Python playback sender → WebSocket PCM → ESP32 I2S TX → MAX98357A → speaker
```

The capture path and inference run as separate tasks so model work and networking do not run in the microphone capture path. A bounded queue and ring history limit memory use and make capture gaps observable; they cannot guarantee lossless operation under arbitrary load.

## 4. Model training and exported artifact

### Model identity

The firmware embeds the same model as the deployment package at `NEW BETTER MODEL/hello_tors_esp32s3_deploy/deploy_esp32s3/export/hello_tors_kws_int8.tflite`.

| Property | Value |
|---|---|
| Task | Custom keyword spotting: “Hello Tors” |
| Format | INT8 TFLite, executed with TensorFlow Lite Micro |
| Model file size | 36,744 bytes |
| SHA-256 | `8db41aecf814eaf64ab1db9da7ac8c29dd613061e1006927e22667211bdb9d1f` |
| Input tensor | `[1, 49, 40]`, INT8; scale `0.5583019853`, zero point `93` |
| Output | Two-class INT8 output; index 1 is interpreted as the keyword score |

The model package and `ML MODEL SPECS/hello_tors_outputs` contain model/export artifacts, feature configuration, test material, and a validation report. The checked-in Python tools cover inference/reference checks and firmware-table generation; a reproducible model-training script is not present in the inspected deployment package. Accordingly, this report describes the ML model as supplied by the ML workflow/team, not as retrained by the ESP32 firmware project.

The supplied validation report is based on speaker/file-disjoint clips. It reports 43.26% true-positive rate and 0 false activations among 742 negative windows at threshold 0.85; its threshold sweep reports 72.7% TPR and 0 false activations at 0.469. These are offline, per-window figures—not continuous-microphone or device results. The report text refers to 0.85 as the firmware threshold, while the current firmware setting is 0.45. The current setting was explicitly selected for the device trial and must be evaluated afresh on the INMP441.

The package also describes successful Realme Buds tests with input conditioning. That does not establish equivalent performance on the INMP441: the current firmware intentionally does not apply optional gain, AGC, denoising, noise injection, or DC filtering to the model input.

## 5. Firmware model deployment

### Required model input processing

The model accepts MFCC features, not raw PCM. Firmware therefore implements the model’s required feature frontend; removing MFCC would make the input incompatible with the trained model. The path is:

1. Read 16 kHz, mono I2S microphone samples and convert the 32-bit I2S words to signed PCM16.
2. Convert PCM16 to the model’s normalized floating-point input (`PCM / 32768`).
3. Form 480-sample windows every 320 samples (30 ms window, 20 ms hop); apply the package’s periodic Hann window and 512-point FFT.
4. Calculate magnitude spectrum, 40 HTK mel bands from 20–8000 Hz, `log(mel + 1e-6)`, and the package-scaled DCT to produce 40 MFCCs.
5. Keep 49 chronological feature frames and quantize them using the exported scale and zero point.
6. Run the unchanged INT8 model with TFLite Micro.

This is required feature extraction, not an additional learned model or optional audio enhancement. Host checks documented in `docs/EXACT_MODEL_INTEGRATION.md` compare the streaming frontend with supplied reference vectors and test score decoding/decision behavior.

### Trigger policy and scheduling

- Firmware threshold: **0.45** (the user-selected trial value; package default is 0.50).
- Trigger rule: **two consecutive evaluated windows** above threshold.
- Refractory period: **1 second**.
- Inference stride is measured/adapted at boot so inference is not blindly run on every 20 ms capture frame. This reduces scheduling pressure but can increase detection delay and affects the temporal decision interval.
- Boot validates model/tensor contracts and runs positive and negative self-tests; initialization is intended to fail rather than listen if these checks fail.

The threshold, smoothing, and cadence are firmware policy, not changes to model weights. The observed history includes both false detections during silence and missed detections when the phrase was spoken. Accuracy is therefore still an open validation item.

## 6. Device, capture, and resource deployment

| Component | Current implementation |
|---|---|
| MCU | ESP32-S3 N16R8; ESP-IDF project |
| Flash / external RAM | 16 MB flash configuration; octal PSRAM enabled for N16R8 |
| Microphone | INMP441, left slot (`L/R` grounded), 16 kHz mono I2S |
| Mic pins | BCLK GPIO4, WS/LRC GPIO5, data GPIO6 |
| Audio frame | 320 samples / 20 ms / 640 bytes of PCM16 |
| KWS arena | 114,688 bytes (112 KiB), internal RAM; included in static data+BSS |
| Model weights | 36,744 bytes in firmware flash |
| Audio history | 1.2 seconds in PSRAM; KWS queue holds up to 320 ms |

The latest documented speaker-playback build reports **163,668 bytes internal static data+BSS** and **96,064 bytes external static BSS**. Their sum is 259,732 bytes (about 253.6 KiB), before dynamically allocated objects, runtime stacks, allocator overhead, and other runtime use. The arena is already included in the internal static figure and must not be added again. The application binary is documented as 1,227,280 bytes; this is flash usage, not RAM usage.

These are link/build figures, not a complete live-memory measurement. They do not demonstrate compliance with the problem’s under-256-KB RAM limit. Nor is under-10% idle CPU demonstrated: no valid current idle-CPU benchmark is recorded. A historical, earlier-build serial snapshot reported approximately 119,744 µs / 19.2 million cycles per inference and only 7.7–9.3 KB free internal heap; it used an older arena/build and must not be treated as the current PSRAM firmware measurement.

## 7. WebSocket backend integration

### Device-to-backend audio

The ESP32 uses a persistent WebSocket connection and waits for backend `hello_ack`. After an accepted KWS event, it sends a `start` control message, then pre-trigger history followed by live microphone frames, and finally a `stop` message when the energy-based endpoint detects ambient quiet. Current documented streaming uses an 800 ms pre-buffer. The energy endpoint is not a trained VAD: loud noise may keep a session open and quiet speech may end it early.

Each binary message has a 16-byte little-endian header (`<BBBBHHII>`) plus 640 bytes of PCM16 payload. The format is 16 kHz, mono, signed 16-bit little-endian PCM; each payload represents 20 ms. Control messages are JSON text. No MFCC features, WAV header, or audio codec is sent over the live microphone stream.

### Python backend roles

The repository contains two distinct backend paths:

1. **ASR backend** in `SERVER SIDE CODE/sih-voice-activator-main`: validates the protocol, accepts `hello/start/audio/stop`, reconstructs per-stream WAV files, queues PCM to a Vosk ASR worker, logs ASR results, and persists device telemetry as JSONL. Its roadmap labels ESP32 hardware integration (M3) as in progress. The available latency helper measures a start-to-first-frame proxy; no measured detection-to-server p50/p95/p99 result is included.
2. **Local test backend** in the project root (`local_backend.py`): receives and saves microphone streams and acknowledges metrics without requiring the ASR service. Optional `--play-wav` and `--test-tone` modes send paced speaker-test audio after device connection. This is a test harness, not the ASR backend.

The device also hosts a read-only local diagnostics page at its own LAN IP (`/`, `/logs`) and saved recording access at `/recordings`. Device metrics are also sent over WebSocket for backend logging. The diagnostics page is unauthenticated HTTP intended for a trusted LAN only; it should not be exposed to the public Internet. Firmware’s configured backend address is environment-specific and must match the laptop/server’s reachable LAN address. `0.0.0.0` is a server bind address, not an ESP32 destination.

## 8. Playback and local recordings

The ESP32 accepts backend PCM playback and has an I2S transmit path for a MAX98357A. The latest documented firmware uses a 64-frame PSRAM queue and waits for five packets (100 ms) before starting playback, with bounded waiting/backpressure and playback counters. Speaker samples are scaled to 70% linear PCM amplitude. The root local test server can send the repository’s sample converted to 16 kHz mono PCM16 or a generated test tone; it sends paced 320-sample packets, not OGG/WAV containers.

Playback is implemented in firmware and host-tested, but successful acoustic output has not been established. The user reported bassy/noisy playback and measured approximately 2.5 V between amplifier VIN and GND despite intending to power it from 5 V. Treat the amplifier supply/wiring and observed sound as unresolved hardware issues; do not present speaker playback as a verified demo.

KWS-triggered PCM clips can also be saved locally in flash: up to three completed clips are retained, with a 30-second cap per clip. These are available through `/recordings` as WAV. USB button recording is a separate legacy/testing path and is not the KWS-to-WebSocket transport.

## 9. Logging and validation evidence

Every five seconds, serial and the ESP32 local diagnostics page report labeled summaries including:

- **RAM / PSRAM:** free, minimum-free, largest block, static data+BSS, arena reserved/used, and PSRAM allocation figures.
- **MIC:** captured frames, frame rate, PCM byte count, RMS/ambient RMS, and successful WebSocket frame writes.
- **KWS:** latest/peak score, input level, stride, self-test status, hit/inference count, average inference time/cycles, and heap samples around inference.
- **HEALTH / PLAY:** DMA, queue, stream and read errors; task stack watermarks; playback receive/submission/queue/overflow/underrun-related counters.

These counters make failures diagnosable but do not by themselves measure accuracy, idle CPU, acoustic output, or confirmed server receipt. In particular, `WS_sent` represents successful device-side WebSocket writes, not proof that the backend stored or transcribed every frame.

Repository documentation records host tests for model/frontend parity, firmware settings, protocol/backend behavior, bounded recording storage, diagnostics, audio conversion, and speaker queue/task behavior. The playback test notes 30 playback tests and a host loopback of 998 packets. These checks validate software logic and host transport; they are not physical-device acoustic or end-to-end latency measurements. No new hardware run was performed while preparing this report.

## 10. Current status and remaining work

| Area | Status |
|---|---|
| Custom “Hello Tors” model artifact | Present, INT8, embedded unchanged in firmware |
| ESP32-S3 KWS and feature frontend | Implemented; host parity/self-tests documented |
| Continuous mic capture and independent KWS scheduling | Implemented with bounded buffers and diagnostics |
| Trigger-gated PCM WebSocket streaming | Implemented; backend protocol and local test receiver available |
| Server WAV reconstruction and Vosk integration | Implemented in Python backend; end-to-end device/ASR acceptance remains to be demonstrated |
| Local metrics and WAV recordings | Implemented; LAN-only diagnostics and retention limits documented |
| Backend-to-device PCM playback | Firmware and test sender implemented; real amplifier/speaker result unresolved |
| Keyword detection/false activation on INMP441 | Not established; earlier user tests showed both misses and false activations |
| <256 KB RAM and <10% idle CPU | Not demonstrated; current build-map totals leave negligible margin before dynamic runtime use |
| Detection-to-first-server-frame p50/p95/p99 | Not measured; backend currently has a proxy, not the full synchronized metric |

Recommended evaluation before claiming completion:

1. Freeze the model hash, firmware build, microphone placement, threshold, and test protocol.
2. Log repeated labeled “Hello Tors,” non-keyword speech, and multi-hour silence/noise trials; calculate detection rate and false activations per hour.
3. Measure live internal and PSRAM heap, largest blocks, stack margins, arena use, and total RAM under idle, Wi-Fi, streaming, recording, and playback.
4. Measure idle CPU over a defined interval and profile inference cadence/latency under continuous 50-frame/s capture.
5. Add synchronized T0 (keyword end) and T3 (first audio frame received) timestamps; collect at least 100 trials and report p50/p95/p99.
6. Run the laptop ASR backend and device on a reachable network; verify a detected phrase produces complete PCM/WAV and a useful transcript. Separately verify the MAX98357A supply and sound with a known test tone.

## 11. References in this repository

- [Exact model/frontend integration](EXACT_MODEL_INTEGRATION.md)
- [ESP32-S3 ML deployment review](ESP32S3_ML_DEPLOYMENT_REVIEW.md)
- [N16R8 PSRAM placement](N16R8_PSRAM.md)
- [KWS streaming test and endpoint behavior](KWS_STREAMING_TEST.md)
- [ESP32/backend protocol contract](ESP32_BACKEND_DEVICE_CONTRACT.md)
- [Local diagnostics and recordings](LOCAL_DIAGNOSTICS.md)
- [Speaker playback implementation and test status](SPEAKER_PLAYBACK_FIX.md)
- [Local playback test server](LOCAL_PLAYBACK_TEST.md)
- [Python backend protocol](../SERVER%20SIDE%20CODE/sih-voice-activator-main/PROTOCOL.md)
- [Python backend README and roadmap](../SERVER%20SIDE%20CODE/sih-voice-activator-main/README.md)
