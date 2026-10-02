# Hello Tors: supplied model and exact frontend

Implemented September 27, 2026 in the main ESP-IDF project, not the separate
`mini_projects/usb_mic` project. `record.py` and network credentials/URI were not changed.

The memory numbers below describe the exact-frontend integration before the
subsequent LED/local-recording addition. Current behavior, artifact and resource
figures: [LED states and local recordings](LED_STATES_AND_RECORDINGS.md).

## Model and threshold

The supplied `NEW BETTER MODEL/hello_tors_esp32s3_deploy/deploy_esp32s3` package's
36,744-byte model is byte-identical to the already embedded model. SHA-256:
`8db41aecf814eaf64ab1db9da7ac8c29dd613061e1006927e22667211bdb9d1f`.
Weights, output interpretation, and quantization are unchanged.

The package's default threshold is **0.50**. The deployed firmware setting is
**0.45**, explicitly confirmed by the user for the current trial. The decision
requires **two consecutive** evaluated windows at/above that threshold, replacing
two-of-three. Cooldown is 1000 ms. Consecutive means evaluations, not microphone
frames; actual evaluation spacing is logged as `stride`.

This threshold has not yet been validated for false accepts/rejects on this INMP441.
The package reports good earbud results with level/noise conditioning. Those results
do not establish accuracy on our unconditioned microphone.

## Required preprocessing only

PCM16 / 32768 -> periodic Hann -> 512-point FFT magnitude -> supplied HTK mel
weights -> log(mel + 1e-6) -> supplied TensorFlow-scaled DCT -> round-half-even
int8 quantization. Input is [1,49,40], scale 0.5583019852638245, zero point 93.
This model cannot take raw PCM instead of these features.

No gain, DC blocker, injected noise, AGC, or denoiser was added. The existing
AC-RMS calculation measures energy for stream endpointing; it does not filter
audio entering the model or backend.

Imported `kws_features.c/h`, `kws_feature_tables.h`, and `kws_test_vectors.h`
from the package. Only the feature function's scratch storage was made static
to avoid a 5284-byte stack allocation; it is single-owner/non-reentrant.
Floating-point contraction is disabled for this file. `kws_frontend.c` caches
int8 rows instead of float rows; tests compare every row against the supplied
float streaming implementation across 150 hops.

## Scheduling and boot checks

Boot runs positive and negative PCM golden vectors through both features and
the actual TFLM interpreter. Feature tolerance: at most 19 one-step differences
out of 1960 and none larger. Both output classes must be within two int8 steps
of the supplied output. Failure prevents listening.

The model's nine operators are explicitly registered. Input/output shapes,
types and quantization are checked. The ordinary MicroInterpreter replaces the
recording interpreter. The arena is 114688 bytes (112 KiB), compared with the
previous reservation of 131072 bytes and observed use of 108508 bytes.
Actual new arena usage and fit must be confirmed by the boot checks.

Inference spacing uses measured boot feature/invoke time, 25% margin and one
RTOS tick for the existing task yield. It increases if runtime inference is
slower. This is a throughput budget, not proof of <10% CPU. Independent
20-ms capture, the 320-ms inference queue, prebuffer, trigger LED, ambient
endpointing, WebSocket protocol, startup beeps and speaker playback remain.
Timing stalls can still overflow bounded queues; inspect HEALTH counters.

## Memory evidence

Successful local ESP-IDF build:

| Item | Before | After |
| --- | ---: | ---: |
| Static data + BSS | 223740 B | 211964 B |
| Reserved tensor arena (already in BSS) | 131072 B | 114688 B |
| Model weights | 36744 B | 36744 B |

Net static reduction: **11776 B (11.5 KiB)**. Feature scratch/twiddles consume
part of the arena savings. Test vectors/tables are in mapped flash.
New application binary: 1201344 bytes; the 10 MiB app partition has 89% free.
Linker DIRAM remaining is 49457 bytes; this is **not** the runtime free heap.

Previous live free heap was only 7744-9260 bytes. New live free heap, largest
block, arena use, stack margins and CPU utilization are not measured until
this firmware runs on the board. Do not claim the total <256 KiB RAM or <10%
CPU competition limits are met: static data plus previous dynamic allocations
already exceed that RAM budget.

## Logs and device validation

Every five seconds, serial and the existing local HTTP page show latest
`score`, interval `peak`, `rms100ms_peak` in dBFS, evaluation `stride`,
`selftest=PASS`, RAM and microphone/drop counters. The level metric is the
maximum RMS of non-overlapping 100-ms blocks consumed by KWS during this
reporting interval, without subtracting DC. -120 dBFS is the silence/no-block
sentinel. Interval peaks reset after reporting. These fields also go out as
WebSocket metrics; no idle audio is uploaded.

From the **root project**, in an ESP-IDF terminal:

```powershell
idf.py -p COM21 flash monitor
```

1. Require `SELFTEST positive: PASS` and `SELFTEST negative: PASS`.
2. Wait for Wi-Fi/backend readiness and quiet ambient calibration.
3. Say Hello Tors several times; inspect peak scores, input dBFS, red LED and upload.
4. Check MIC remains approximately 50 fps, all overflow/error counters stay zero,
   and heap/largest-block/stack headroom remain stable with HTTP logging open.
5. Confirm ambient noise ends the upload and backend speaker playback works.
6. Run labeled speech/noise trials for detection/false-activation measurements.

No board was flashed or hardware recognition/latency verified during this change.
The historical frontend-only artifact is `bin/EdgeAIKWS_HelloTors_ExactFrontend_T045.bin`, offset
`0x10000`; prefer `idf.py flash` for the matching bootloader/partition table.

## Reproducible host checks

```powershell
gcc -std=c11 -O2 -Wall -Wextra -Werror -msse2 -mfpmath=sse -Imain tests/kws_frontend_test.c main/kws_features.c main/kws_frontend.c -lm -o tests/kws_frontend_test.exe
./tests/kws_frontend_test.exe
python tests/test_kws_firmware_integration.py
python tests/test_firmware_settings.py
python tests/test_local_backend.py
node tests/test_diagnostics_page.cjs
idf.py build
idf.py size
```

Host checks found both golden vectors **1960/1960 exact**, streaming parity,
ties-to-even/saturation and consecutive/cooldown/stride behavior passing.
They do not execute TFLM on an ESP32. Existing PCM conversion, endpoint/history,
diagnostic-store and USB mini-project tests also remain applicable.

Pre-change main sources are backed up in
`saved/kws_before_exact_frontend_2026-09-27/`.

For a standalone document to send to the ML team, see
[ESP32-S3 ML deployment review](ESP32S3_ML_DEPLOYMENT_REVIEW.md).
