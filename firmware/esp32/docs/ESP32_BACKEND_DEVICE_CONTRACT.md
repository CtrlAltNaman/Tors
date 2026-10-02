I (383) heap_init: At 3FCE9710 len 00005724 (21 KiB): RAM
I (388) heap_init: At 3FCF0000 len 00008000 (32 KiB): DRAM
I (394) heap_init: At 600FE000 len 00001FE8 (7 KiB): RTCRAM
I (400) spi_flash: detected chip: boya
I (402) spi_flash: flash io: dio
I (406) sleep_gpio: Configure to isolate all GPIO pins in sleep state
I (412) sleep_gpio: Enable automatic switching of GPIO sleep configuration
I (419) main_task: Started on CPU0
I (459) main_task: Calling app_main()
Failed to resize buffer. Requested: 102608, available 59764, missing: 42844
E (469) hello_tors_kws: TFLite tensor allocation failed; arena=65536 bytes
E (469) edgeai_kws: Hello Tors KWS initialization failed: ESP_FAIL
# ESP32-S3 ↔ Backend Device Contract

## Purpose

This document defines the audio and telemetry contract between the ESP32-S3 voice device and the Python backend.

The device performs local keyword spotting for the custom phrase **“Hello Tors”**. It must not continuously upload microphone audio. After a local detection, it sends the buffered and subsequent audio to the backend for ASR processing.

The device also accepts audio from the backend and plays it through the MAX98357A amplifier and speaker.

## Device audio configuration

| Property | Value |
|---|---:|
| Sample rate | 16,000 Hz |
| Channels | 1 / mono |
| Sample format | signed 16-bit little-endian PCM |
| Transport frame | 20 ms |
| Samples per frame | 320 |
| PCM bytes per frame | 640 |
| Keyword | `Hello Tors` |

The KWS model uses 30 ms MFCC frames with a 20 ms step. The model input is 49 × 40 INT8 MFCC values, representing approximately one second of audio.

## WebSocket connection

The ESP32 connects to the backend using WebSocket:

```text
ws://<backend-host>:8765
```

For a deployed backend, use a public hostname and `wss://` with TLS. The current development server listens on `0.0.0.0:8765`.

The WebSocket should remain connected while the device is listening. Reconnecting only after keyword detection adds avoidable latency and can lose the first audio frames.

## Connection lifecycle

The device sends one `hello` message after the WebSocket connects:

```json
{
  "type": "hello",
  "proto": 1,
  "device_id": "edgeai-esp32s3",
  "codecs": ["pcm_s16le"],
  "sample_rate": 16000
}
```

The backend replies:

```json
{
  "type": "hello_ack",
  "proto": 1
}
```

The device must wait for `hello_ack` before sending an audio stream.

## Microphone upload flow

The device continuously listens locally and runs the `Hello Tors` KWS model. It maintains a rolling pre-buffer, normally 1–1.5 seconds of PCM audio.

When the KWS decision is positive:

1. The device records the detection timestamp.
2. The device sends `start`.
3. The device sends the pre-buffered audio.
4. The device continues sending live microphone audio.
5. The device stops after local silence detection, a timeout, or a maximum stream duration.

The `start` message is:

```json
{
  "type": "start",
  "stream_id": 42,
  "codec": "pcm_s16le",
  "sample_rate": 16000,
  "channels": 1,
  "frame_ms": 20,
  "prebuffer_ms": 1200,
  "live_sample_offset": 19200,
  "t_detect_ms": 1734567890123
}
```

### `start` fields

| Field | Meaning |
|---|---|
| `stream_id` | Unique ID for this audio capture session |
| `prebuffer_ms` | Duration of audio preceding detection included in the stream |
| `live_sample_offset` | Sample offset where live audio begins |
| `t_detect_ms` | SNTP-synchronized Unix timestamp at local keyword detection |

The backend must accept `prebuffer_ms`, `live_sample_offset`, and `t_detect_ms` even if the first implementation does not use all of them.

## Binary microphone frame format

Every microphone audio frame is one WebSocket binary message:

```text
16-byte header + 640-byte PCM payload
```

The header is packed little-endian using:

```text
struct format: <BBBBHHII
```

| Offset | Size | Field | Value |
|---:|---:|---|---|
| 0 | 1 | Magic | `0xA5` |
| 1 | 1 | Protocol version | `1` |
| 2 | 1 | Codec | `0` = `pcm_s16le` |
| 3 | 1 | Flags | See below |
| 4 | 2 | Sequence | Starts at `0`, increments by one |
| 6 | 2 | Payload length | `640` |
| 8 | 4 | Stream ID | Matches `start.stream_id` |
| 12 | 4 | Sample offset | `0`, `320`, `640`, ... |

The payload contains exactly 320 signed 16-bit little-endian PCM samples.

Flags:

| Flag | Value | Meaning |
|---|---:|---|
| `FIRST` | `0x01` | First frame in the stream |
| `LAST` | `0x02` | Final frame in the stream |
| `PREBUF` | `0x04` | Frame belongs to the pre-detection buffer |

The first frame must include `FIRST`. Pre-buffer frames should include `PREBUF`. The final frame includes `LAST`; if it is shorter than 320 samples, the device zero-pads it to 640 bytes.

## Stopping a microphone stream

The device sends:

```json
{
  "type": "stop",
  "stream_id": 42,
  "reason": "silence"
}
```

Possible reasons include:

```text
silence
timeout
network_error
button
```

The backend should finalize the received PCM as mono, 16 kHz, 16-bit WAV and pass the audio to ASR.

The backend should not treat every non-empty ASR result as a keyword detection. Keyword detection happens locally on the ESP32. ASR text and local KWS events are separate signals.

## Online device metrics

Metrics are sent as WebSocket text JSON messages. They should be sent periodically, for example every 5–10 seconds, and once immediately after each keyword event.

The backend should accept:

```json
{
  "type": "metrics",
  "device_id": "edgeai-esp32s3",
  "model": "hello_tors_int8",
  "uptime_ms": 123456,
  "model_flash_bytes": 36744,
  "tensor_arena_bytes": 65536,
  "tensor_arena_used_bytes": 42112,
  "free_heap_before_bytes": 210432,
  "free_heap_after_bytes": 207880,
  "minimum_free_heap_bytes": 201104,
  "inference_count": 1000,
  "inference_cycles_last": 185000,
  "inference_cycles_avg": 181500,
  "inference_us_avg": 1134,
  "idle_cpu_percent": 3.8,
  "keyword_hits": 12,
  "false_activation_count": 0,
  "dropped_trigger_count": 1
}
```

The ESP32 should also print these metrics using `ESP_LOGI` so they are available through the USB serial monitor.

### Metric definitions

| Metric | Definition |
|---|---|
| `model_flash_bytes` | Size of the embedded INT8 TFLite model; current model is 36,744 bytes |
| `tensor_arena_bytes` | Reserved TFLite Micro working memory |
| `tensor_arena_used_bytes` | Actual allocator high-water mark, if available |
| `free_heap_before_bytes` | Free heap immediately before inference |
| `free_heap_after_bytes` | Free heap immediately after inference |
| `minimum_free_heap_bytes` | Lowest free heap observed since boot |
| `inference_cycles_avg` | Average CPU cycles used by MFCC plus model inference |
| `idle_cpu_percent` | CPU time used by continuous KWS divided by wall-clock time |
| `keyword_hits` | Number of accepted local KWS activations |
| `false_activation_count` | Activations observed during a labeled silence/noise test |
| `dropped_trigger_count` | Keyword events not streamed because the backend was unavailable |

The backend should log metrics with device ID and timestamp and preserve them for evaluation.

## Latency measurement

The device synchronizes its clock using SNTP. On detection it sends `t_detect_ms` in the `start` message.

The backend records:

```text
T0 = start.t_detect_ms
T3 = timestamp when the first binary audio frame is received
latency_ms = T3 - T0
```

The backend should report p50, p95, and p99 latency over at least 100 keyword events.

If clock synchronization is not available, the backend may report the proxy measurement from receiving `start` until receiving the first binary frame, but it must label it as a proxy.

## Backend-to-device speaker playback

The backend can request playback using:

```json
{
  "type": "play_start",
  "audio_id": 42,
  "codec": "pcm_s16le",
  "sample_rate": 16000,
  "channels": 1,
  "frame_ms": 20
}
```

The backend then sends binary PCM frames using the same 16-byte header format. For playback frames:

- `stream_id` is replaced by `audio_id`.
- Codec is `0` (`pcm_s16le`).
- Payload is 640 bytes / 320 samples.
- `FIRST` marks the first playback frame.
- `LAST` marks the final playback frame.
- `PREBUF` is not required for playback.

The device sends or receives the following completion message after playback:

```json
{
  "type": "play_stop",
  "audio_id": 42,
  "reason": "complete"
}
```

The ESP32 converts each received 16-bit PCM sample to the MAX98357A I2S transmit format and writes it to the speaker output.

## Hardware audio connections

### INMP441 microphone

| INMP441 | ESP32-S3 |
|---|---:|
| SCK / BCLK | GPIO 4 |
| WS / LRC | GPIO 5 |
| SD | GPIO 6 |
| L/R | GND / left slot |
| VDD | 3.3 V |
| GND | GND |

### MAX98357A amplifier

| MAX98357A | ESP32-S3 / power |
|---|---:|
| BCLK | GPIO 4 |
| LRC | GPIO 5 |
| DIN | GPIO 8 |
| SD | 3.3 V for enabled output |
| GAIN | Floating/default gain |
| VIN | 5 V recommended, within board limits |
| GND | Common GND |

The microphone and amplifier share BCLK, LRC, and ground. The microphone uses GPIO 6 for I2S input and the amplifier uses GPIO 8 for I2S output.

## Error handling

- If Wi-Fi or WebSocket is disconnected, the device continues local KWS and increments `dropped_trigger_count` when a detected event cannot be uploaded.
- The device must not block the continuous KWS task on backend transmission.
- The backend should reject malformed frames without terminating the whole server.
- The backend should enforce a maximum stream duration of 10 seconds.
- The device should stop playback on malformed, incomplete, or timed-out speaker audio.
- Metrics must never be allowed to block microphone capture or KWS inference.

## Current implementation status

Implemented in the project:

- INMP441 capture at 16 kHz mono
- PCM conversion
- 20 ms PCM framing in the saved WebSocket client implementation
- MAX98357A I2S transmit path for startup beeps
- Python backend PCM frame parser and ASR worker

Implemented in the current firmware/backend integration:

- TFLite Micro runtime integration with the embedded INT8 model
- On-device MFCC extraction using the exported 16 kHz / 40-MFCC configuration
- Local `Hello Tors` detection with score smoothing and cooldown
- Rolling 1.2-second pre-buffer
- Automatic KWS-triggered WebSocket streaming with 20 ms PCM frames
- Periodic device metrics over WebSocket and JSONL persistence on the backend
- Backend-to-device PCM playback framing for the MAX98357A speaker
- Device `play_stop` completion messages
- Server-side start-to-first-frame proxy latency samples with p50/p95/p99 summary

Hardware validation is still required to tune the threshold and confirm the
feature extraction matches the training pipeline exactly. The current
firmware reports these runtime values over USB logs: model size, tensor arena
reserved/used bytes, heap before/after inference, minimum heap, inference
cycles/time, scores, keyword hits, and false-activation counter.
