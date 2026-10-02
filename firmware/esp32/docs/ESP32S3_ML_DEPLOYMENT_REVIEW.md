# Hello Tors on ESP32-S3: deployment handoff for ML review

Date: 2026-09-27. Scope: the main EdgeAIKWS firmware with INMP441 microphone,
MAX98357A speaker and KWS-triggered WebSocket streaming. This is not the separate
USB-microphone mini-project.

Update: this firmware now includes mutually exclusive red/green LED states and
three persistent KWS-triggered local recordings at `/recordings`, capped at 30
seconds each. This adds flash writes and maintenance pauses, not ML preprocessing.
See [LED states and recordings](LED_STATES_AND_RECORDINGS.md) for resource costs
and verification requirements.

## 1. Summary and requested review

We deploy the supplied int8 TFLite model unchanged. The ESP32 computes the
package's required MFCC input, invokes TensorFlow Lite Micro locally, and applies
a temporal decision rule before starting an audio stream.

**Current threshold: 0.45, explicitly confirmed by the project owner.** The
package default was 0.50. We require two consecutive positive evaluations and
use a one-second cooldown. These are firmware decision settings, not changed
model weights. They require validation on this microphone.

No gain, DC-blocking filter, noise injection, denoising, AGC, extra model,
retraining, pruning or re-quantization is applied in this integration.
Required MFCC extraction and PCM unit conversion are still present: this
exported model accepts features, not a raw waveform.

Please verify the tensor/feature specifications below and the review checklist
in section 10. Successful compilation or golden-vector tests do not establish
physical-microphone accuracy, false-activation rate or competition compliance.

## 2. Model identity and runtime

Source package:
`NEW BETTER MODEL/hello_tors_esp32s3_deploy/deploy_esp32s3/`

Model: `export/hello_tors_kws_int8.tflite`

- Size: **36744 bytes**.
- SHA-256: `8db41aecf814eaf64ab1db9da7ac8c29dd613061e1006927e22667211bdb9d1f`.
- Embedded as a constant byte array in `main/kws_model_data.h`, linked into flash.
- The new package's model bytes are identical to the model already embedded in
  this project. The important integration correction is the feature frontend.
- No model download from the backend and no on-device training.
- ESP-IDF 5.5.2; locked components: `esp-tflite-micro` 1.4.1, `esp-nn` 1.4.1,
  and `esp_websocket_client` 1.8.0. ESP-NN optimizations are enabled.
- ESP32-S3, CPU configured at 160 MHz, 16 MB flash configuration.
- Runtime: `tflite::MicroInterpreter`, with a statically reserved, 16-byte-aligned
  112 KiB tensor arena. No recording interpreter in the current inference path.

Registered operators: SHAPE, STRIDED_SLICE, PACK, RESHAPE, CONV_2D,
DEPTHWISE_CONV_2D, MEAN, FULLY_CONNECTED and SOFTMAX.

Boot checks model schema and tensor contracts, allocates tensors, then runs
positive and negative self-tests. An allocation, contract or self-test failure
prevents normal listening instead of proceeding with invalid inference.

## 3. Physical microphone to model input

```text
INMP441 left slot -> I2S 32-bit words -> signed PCM16
                  -> divide by 32768 -> package MFCC -> int8 tensor
                  -> unchanged TFLM model -> dequantized keyword score
                  -> 0.45 + consecutive positives + cooldown -> stream request

Captured PCM16 also feeds a bounded history buffer and an energy measurement.
WebSocket uploads use the PCM16, not MFCCs or conditioned audio.
```

### Capture and sample conversion

| Item | Current implementation |
| --- | --- |
| Microphone | INMP441, L/R grounded, LEFT slot |
| Sample rate | 16000 Hz |
| I2S format | Philips standard, 32-bit mono receive slots |
| Pins | BCLK GPIO4, WS/LRC GPIO5, mic data input GPIO6 |
| PCM conversion | `pcm[i] = (int16_t)(i2s_word[i] >> 16)` |
| Chunk | 320 samples = 20 ms = 640 PCM bytes |
| Float conversion | `float_sample = pcm[i] / 32768.0f` |

The shift selects the upper 16 bits of each received word; it is not an added
gain stage. PCM division is fixed conversion to approximately [-1,1), **not**
per-recording peak normalization, automatic gain or RMS normalization.
Actual microphone bit alignment and signal level still need hardware checking.

### Required feature pipeline

| Stage | Exact setting |
| --- | --- |
| Framing | 480 samples (30 ms), hop 320 samples (20 ms) |
| Window | Periodic Hann from supplied generated table |
| FFT | 512 points, trailing zero padding |
| Spectrum | Magnitude `sqrt(real^2 + imag^2)`, not power |
| Mel | 40 bands, supplied TensorFlow/HTK weights, 20 to 8000 Hz |
| Compression | Natural logarithm `log(mel + 1e-6)` |
| MFCC | Supplied TensorFlow-scaled DCT, all 40 coefficients |
| Feature order | 49 frames, oldest first; 40 coefficients per frame |
| Input tensor | Shape `[1,49,40]`, int8, 1960 bytes |
| Input quantization | Scale `0.5583019852638245`, zero point `93` |

Quantization is:

```text
q = clip(round_half_even(mfcc / 0.5583019852638245 + 93), -128, 127)
```

Rounding occurs **after** adding the zero point. No extra feature centering,
standardization, coefficient dropping, deltas or reordering is applied.

The 49 frames span 15840 samples (990 ms). The live implementation takes the
latest 480 samples for each new hop: 160 old plus 320 new. It shifts the feature
history and appends one row. After a reset, inference waits for 50 captured hops
(one second), so the initial partially zero-padded frame has left the window.
At that point the features cover the last 15840 of the first 16000 samples.
This follows the supplied streaming implementation; it is not inference on
49 independent 20-ms frames.

### Changes relative to the previously integrated frontend

The earlier firmware used symmetric Hann, power spectrum, approximate integer-bin
mel filters and a differently scaled DCT/quantization path. Those are replaced
by the supplied frontend. Changing the threshold alone would not correct those
feature mismatches.

## 4. Output score and detection policy

Output is int8 `[1,2]`, scale `1/256`, zero point `-128`. Index 1 is treated as
the keyword class. The model already contains Softmax; we do not apply Softmax
again or interpret the output as logits.

```text
keyword_score = (output_int8[1] + 128) / 256
positive = keyword_score >= 0.45
```

Representable scores are spaced by 1/256. At this threshold, raw output `-13`
gives 0.44921875 and is negative; raw output `-12` gives 0.453125 and is positive.

- Two **consecutive evaluated windows** must be positive. A negative resets the
  consecutive count. This is neither a moving-average score nor two-of-three.
- At least 1000000 microseconds must have elapsed since the previous trigger.
  The implementation uses ESP32 monotonic time; the supplied reference expresses
  its cooldown in audio hops.
- Capture gaps, completed streams and playback clear the feature window and
  positive streak. Cooldown history is retained.
- The application's trigger handler resets the window after each detection,
  including when the backend is unavailable.
- No energy/RMS gate filters keyword scores after startup calibration.
- Red LED GPIO15 pulses for 300 ms even if the backend cannot accept audio.

Lowering 0.50 to 0.45 may recover weaker detections but may also increase false
activations. This is a trial value, not a claim of a calibrated 45% confidence.

## 5. Scheduling and memory adaptations

These are additions around the model, not modifications to its learned network.

| Adaptation | Implementation and effect |
| --- | --- |
| Independent capture | Core 0, priority 8, 4096-byte stack; no inference or network writes in the capture path |
| Separate inference | Core 1, priority 2, 6144-byte stack |
| Bounded inference queue | 16 microphone frames, 320 ms; nonblocking producer, gaps reset KWS |
| Adaptive evaluation spacing | Boot-measured feature/invoke time, 25% margin, allowance for one RTOS tick per invocation |
| Runtime adjustment | Increases evaluation stride if a slower invocation is measured; no decrease during the same boot |
| Feature cache | 1960-byte int8 history instead of the reference's 7840-byte float feature history; each row quantized once |
| Shared feature scratch | FFT/mel scratch is static rather than 5284 bytes on the task stack; boot then inference task are sole owners |
| Floating-point arithmetic | `-ffp-contract=off` on the feature source to preserve separate multiply/add rounding |
| PCM history | 60 frames, 1.2 s, 38400 bytes of sample storage |
| Tensor arena | 114688 bytes reserved in BSS; included in static RAM, not an additional heap allocation |

Only the scratch storage lifetime differs in the imported feature C implementation;
generated mel/Hann/DCT tables and test vectors are preserved. The compact cache
is tested against the supplied float cache for every hop across 150 hops.

The scheduler's budget is approximately:

```text
feature_budget = ceil(1.25 * measured_feature_us)
available_per_hop = 20000 - feature_budget
stride_hops = max(1, ceil((ceil(1.25 * measured_invoke_us) + tick_us)
                          / available_per_hop))
```

Initialization fails if the feature budget leaves no time. New features are
computed on every consumed hop, but inference happens only every `stride_hops`.
Thus two positives are separated by `stride_hops * 20 ms` of captured audio,
not necessarily 20 ms; queueing may add wall-clock latency. Check the logged
`stride`. This affects accuracy and response delay and needs ML-team validation.
Bounded buffering and timing margin cannot guarantee zero losses under all load.

Before local recording, static data+BSS was **211964 bytes**, down from 223740 bytes.
With LED/local recording support, before PSRAM was enabled, it was **214308 bytes**, plus a new 4096-byte
recorder task stack and other runtime overhead. Model flash is 36744 bytes, but the full
application is about 1.20 MB because it includes runtime, networking, tables,
self-test vectors and diagnostics. Previous observed arena use was 108508 bytes;
new runtime usage/fit must be confirmed at boot. Free heap is not inferable from
model size or static RAM alone.

Update 2026-09-27: user confirmed N16R8 hardware. Octal PSRAM is now enabled;
audio history and upload staging are external BSS, while the 112 KiB arena stays
internal. See [N16R8 PSRAM configuration](N16R8_PSRAM.md) for current placement,
metrics and verification. The above internal-static figure is historical.
LED colors are now red GPIO15 and green GPIO7; model, MFCC, threshold, gain,
stride and CPU frequency are unchanged.

Speaker playback update: internal data+BSS is now 163652 bytes and external
BSS 64768 bytes. See [Speaker playback fix](SPEAKER_PLAYBACK_FIX.md).
No model/frontend/decision changes accompany that buffering fix.

**The complete firmware is not demonstrated to meet <256 KB RAM or <10% idle
CPU.** Static memory plus previous live dynamic allocations already exceeded
the RAM target. Neither adaptive stride nor inference time alone measures total
idle-listening CPU utilization.

## 6. Extra audio/session behavior outside ML

### Startup and idle

Two startup beeps play on MAX98357A (shared GPIO4/5 clocks, data GPIO8). Capture
discards initial settling audio for about 300 ms, then estimates ambient energy
for 100 frames (two seconds). KWS is held off until this calibration is ready.
Calibration changes the upload endpoint threshold, not PCM amplitude or features.
After calibration, the feature window must still warm up.

### On a successful detection

If the WebSocket has received the backend's `hello_ack`, start an audio session.
Send 800 ms of prebuffer and then live PCM16 at 16 kHz mono in 20-ms chunks.
Each binary audio message has a 16-byte application header plus 640 PCM bytes,
before WebSocket/TCP/Wi-Fi overhead. There is no Opus/compression or upload of
MFCC tensors. Control messages and metrics may be sent while idle, but idle
microphone audio is not continuously uploaded.

The prebuffer can include the wake phrase. Firmware does not locate and trim
the exact keyword boundary; this is not guaranteed to send only post-keyword
speech. If disconnected, the upload is dropped, but local flash recording can
still proceed. There is no unlimited offline recording or delayed upload of
that trigger. Local flash writes and backend sending use independent cursors.

### Stopping the upload

The firmware uses an **energy-based endpoint**, not an additional trained VAD:

- Measure AC RMS from each 320-sample PCM chunk (subtract mean only in the
  energy calculation; never alter the PCM buffer).
- Calibrate ambient RMS with a two-second average, then slowly track modest
  background changes during idle.
- Freeze the session threshold at `max(128, ambient_rms * 3)` in PCM units.
- End after at least two seconds of live capture and 75 consecutive frames
  (1.5 s) at or below that threshold.
- Loud non-speech can keep a stream open; soft speech can end it early.
- Network failure or overwritten unsent history can end a stream separately.

KWS pauses during active sessions, local-save completion, flash maintenance and
speaker playback. Backend PCM playback goes
through the existing speaker path; there is no acoustic echo cancellation.
The next listening session starts with a cleared feature window. Existing
button-controlled USB recording and `record.py` remain separate from KWS uploads.

## 7. Verification: what is proven and what is not

Host tests compare features against the supplied positive and negative vectors:
**1960/1960 exact for each vector**. Streaming-cache parity, quantization rounding,
threshold boundaries, consecutive decisions and cooldown behavior are also tested.
ESP-IDF compilation checks the target integration, not physical recognition.

Every boot is programmed to run both vectors through actual on-device TFLM:

| Vector | Expected raw int8 output | Keyword score |
| --- | --- | ---: |
| Positive | `[-122, 122]` | 0.9765625 |
| Negative | `[93, -93]` | 0.13671875 |

Allowed feature error: no differences larger than one int8 step, and at most
19 one-step differences out of 1960. Each output class must be within two int8
steps of the reference. These tolerances are validation tolerances, not extra
runtime smoothing or adjustments to scores.

Require `SELFTEST positive: PASS` and `SELFTEST negative: PASS` in actual boot
logs. Tests use embedded PCM, so they do not test microphone wiring, I2S word
alignment, room acoustics or microphone gain. No new on-board run was performed
while preparing this threshold change and document.

## 8. Diagnostics available for cross-verification

Serial and the ESP32's read-only local HTTP page publish a five-second summary;
WebSocket metrics carry corresponding values to the backend.

- RAM: internal heap used/free/minimum/largest block, static data+BSS, arena
  used/reserved, free heap before/after inference and task stack headroom.
- MIC: captured frames and PCM bytes, frames/s (expected about 50), audio and
  ambient AC RMS, WebSocket frame writes.
- KWS: last score, interval peak score, hit/inference counts, average invocation
  duration/cycles, stride and self-test state.
- Input level: peak RMS of non-overlapping 100-ms blocks consumed by KWS, in
  dBFS. This diagnostic includes DC, unlike endpoint AC RMS; it does not change
  model inputs. -120 dBFS is the silence/no-block sentinel.
- HEALTH: DMA/queue/stream overflows, read errors and backend-unavailable triggers.

Peak metrics reset at reporting time. A five-second last-score snapshot can miss
a brief high score; inspect `peak`. Invocation timing excludes frontend work,
queue wait and network delivery. Successful WebSocket writes are not server
receipt acknowledgements. False-activation counts require external ground truth;
the firmware cannot know whether the user actually said the phrase.

Not established by these logs alone: labeled detection rate, false activations
per hour, total idle CPU percentage, or keyword-end-to-server p50/p95/p99 latency.
Those require a controlled evaluation, timestamps/clock alignment or a defined
round-trip proxy, and server-side receipt instrumentation.

## 9. Files to inspect and how to reproduce

| File | Responsibility |
| --- | --- |
| `main/kws_model_data.h` | Embedded unchanged model |
| `main/kws_features.c`, `main/kws_feature_tables.h` | Supplied feature arithmetic/tables |
| `main/kws_frontend.c/h` | Compact streaming cache, 0.45 threshold, temporal rule, stride budget |
| `main/kws_detector.cc` | TFLM registration, tensor validation, self-tests, invocation and stats |
| `main/audio_conversion.c` | I2S word to PCM16 conversion |
| `main/main.c` | Capture/inference tasks, LED, session lifecycle, playback and logging |
| `main/stream_policy.c/h` | AC RMS, ambient estimate, endpoint and PCM history |
| `main/backend_client.c` | WebSocket control and audio framing |
| `tests/kws_frontend_test.c` | Feature/stream/decision behavioral tests |
| `tests/test_kws_firmware_integration.py` | Model identity and integration checks |

Host verification from project root:

```powershell
gcc -std=c11 -O2 -Wall -Wextra -Werror -msse2 -mfpmath=sse -Imain tests/kws_frontend_test.c main/kws_features.c main/kws_frontend.c -lm -o tests/kws_frontend_test.exe
./tests/kws_frontend_test.exe
python tests/test_kws_firmware_integration.py
python tests/test_firmware_settings.py
```

Build/flash from an ESP-IDF-enabled terminal in the main project root:

```powershell
idf.py build
idf.py -p COM21 flash monitor
```

The current 0.45 app-only artifact is `bin/EdgeAIKWS_T045_LED_LocalRecordings.bin`
(offset `0x10000`). Prefer `idf.py flash` so the matching bootloader and partition
table are installed. Older binaries remain older firmware; their filenames do
not imply the threshold has been updated. No credentials are included in this
handoff; network configuration is separate from the ML contract.

## 10. ML-team cross-verification checklist

- [ ] Confirm the model hash, `[1,49,40]` shape and keyword class index 1.
- [ ] Confirm PCM/32768 scaling and microphone word conversion are appropriate
  for training data; compare actual INMP441 recordings with training levels.
- [ ] Confirm periodic Hann, magnitude (not power), mel range/weights, DCT
  scaling, all 40 coefficients and round-half-even after zero-point addition.
- [ ] Confirm one-second rolling input, 49 frames spanning 990 ms and the live
  160-sample offset agree with the Python live-reference implementation.
- [ ] Approve 0.45, two consecutive evaluations and one-second cooldown for
  this microphone; report both true detections and false activations per hour.
- [ ] Replay reference audio at the device's logged stride, not only at every
  20-ms hop, to quantify the accuracy/latency effect of evaluation spacing.
- [ ] Review the removal of optional DC blocking and all gain/noise conditioning.
  The package reports earbud success with -25 dB gain and -55 dBFS injected
  pink-noise floor; neither is enabled here. Earbud results cannot be assumed
  to transfer directly to unconditioned INMP441 audio.
- [ ] Compare the same captured WAV on the Python pipeline and device pipeline,
  starting from identical PCM samples, then compare features and raw outputs.
- [ ] Validate positive/negative boot outputs before interpreting live scores.
- [ ] Test silence, fans, music, unrelated speech, similar phrases, multiple
  speakers, distances and speaking levels. Keep calibration speech-free.
- [ ] Decide whether the 800-ms prebuffer, keyword inclusion, energy endpoint
  and KWS pause during playback/upload fit the intended interaction.
- [ ] Define how full-firmware RAM, idle CPU and end-to-end latency are measured;
  do not report model-only resource figures as complete system compliance.

Requested ML-team response: approve each item or supply the exact replacement
setting/reference code and expected PCM/features/output vectors for differences.
