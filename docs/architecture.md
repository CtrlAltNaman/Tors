# Tors architecture

## Runtime stages

1. The ESP32-S3 captures 16 kHz mono audio from the INMP441 over I2S.
2. Audio is converted to PCM16 and placed in a bounded rolling history.
3. The feature frontend produces the model MFCC tensor locally.
4. TensorFlow Lite Micro performs int8 inference on the device.
5. A temporal decision rule accepts a keyword event.
6. The device sends a start event, prebuffer, and live PCM frames over WebSocket.
7. The backend validates frames, reconstructs WAV audio, and forwards it to ASR.
8. Device and backend metrics are persisted for evaluation.

## Design boundaries

### Edge runtime

The edge runtime owns capture, feature extraction, inference, decision policy,
prebuffering, transport, and device diagnostics. It must remain bounded in RAM,
avoid blocking capture on network I/O, and fail closed if model allocation or
self-tests fail.

### Model pipeline

Training and export are separate from firmware. A model release must identify
the keyword, sample rate, tensor shape, quantization parameters, model hash, and
evaluation results. Firmware should consume a reviewed artifact, not run
training code or download a model at runtime.

### Backend

The backend owns protocol validation, stream assembly, ASR integration, server
timestamps, and durable evaluation logs. It must reject malformed frames without
terminating unrelated device sessions.

## Failure behavior

- Backend unavailable: continue local KWS and count dropped trigger events.
- Invalid model or tensor contract: refuse normal listening.
- Queue overflow or capture gap: reset the feature window and record a diagnostic.
- Malformed network frame: reject the frame and preserve the session where safe.
- Incomplete stream: finalize only with an explicit reason and mark it partial.

## Current migration status

The current implementation remains under firmware/esp32 so firmware-relative
paths stay stable. Next steps are:

1. Extract the protocol parser and backend into backend/ with contract tests.
2. Move model preparation/export scripts into ml/ while retaining generated
   firmware assets through a manifest and hash.
3. Add host audio replay in simulator/.
4. Keep the ESP-IDF project focused on device code and board configuration.
