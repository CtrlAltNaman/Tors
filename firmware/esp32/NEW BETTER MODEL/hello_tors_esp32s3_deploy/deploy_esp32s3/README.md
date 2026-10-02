# Hello/Tors KWS — ESP32-S3 deployment package

Everything needed to run the `hello_tors_kws_int8.tflite` keyword spotter on an ESP32-S3,
plus the Python reference it was verified against.

```
deploy_esp32s3/
├── firmware/hello_tors_kws/        ESP-IDF project (build + flash this)
│   ├── CMakeLists.txt, sdkconfig.defaults
│   └── main/
│       ├── app_main.cc             mic -> features -> TFLite Micro -> detector -> log/LED
│       ├── kws_config.h            <- EDIT: mic type + pins, gain, threshold, LED
│       ├── kws_features.c/.h       MFCC front end (portable C, verified bit-exact vs TensorFlow)
│       ├── kws_feature_tables.h    generated: Hann window, mel filterbank, DCT (from tf.signal)
│       ├── kws_audio.c/.h          input conditioning: DC block, gain, optional noise floor
│       ├── kws_detector.c/.h       threshold + consecutive-window smoothing + refractory
│       ├── kws_mic.c/.h            I2S standard (INMP441 etc.) or PDM mic capture
│       ├── hello_tors_model_data.h the model as a C array (byte-identical to the .tflite)
│       ├── kws_test_vectors.h      generated: boot self-test clips + expected outputs
│       └── idf_component.yml       pulls espressif/esp-tflite-micro (ESP-NN kernels)
├── export/                         model + feature_config.json + training report
├── scan_audio.py                   Python reference: scan a WAV file
├── mic_kws.py                      Python reference: live mic on the PC
├── tools/
│   ├── gen_firmware_tables.py      regenerates kws_feature_tables.h + kws_test_vectors.h
│   ├── verify_c_features.py        compiles the firmware C front end on the PC, checks it vs Python
│   └── pc_harness/kws_pc_main.c
└── test_audio/                     Naman_Close_clean.wav, buds_test2.wav, self-test clips
```

## 1. The feature pipeline (must match training exactly)

| Step | Value |
|---|---|
| Audio | 16 kHz mono, float in [-1, 1) |
| Window | 15840 samples (0.99 s) → 49 frames |
| Framing | frame 480, step 320 (20 ms), periodic Hann, zero-pad to 512 |
| Spectrum | **magnitude** \|rfft\| (not power), 257 bins |
| Mel | 40 bands, 20–8000 Hz, `tf.signal.linear_to_mel_weight_matrix` (HTK) |
| Log | `log(mel + 1e-6)` |
| MFCC | `tf.signal.mfccs_from_log_mel_spectrograms`, all 40 coefficients |
| Quantise | `round(x / 0.5583019853 + 93)`, clamp to int8 |
| Output | int8 [2], scale 1/256, zero point −128; **index 1 = P("Hello Tors")** |

Librosa's MFCC differs in all three of: power vs magnitude, Slaney vs HTK mel, DCT scaling —
using it makes every window score 0.

## 2. Build and flash

Needs **ESP-IDF v5.3 or newer** (tested API: v5.3 I2S driver; esp-tflite-micro 1.4.x).

1. Edit `firmware/hello_tors_kws/main/kws_config.h`:
   - `KWS_MIC_TYPE` — `KWS_MIC_TYPE_I2S_STD` (INMP441, ICS-43434, SPH0645…) or
     `KWS_MIC_TYPE_PDM` (e.g. XIAO ESP32S3 Sense on-board mic, pins already set).
   - I2S pins `KWS_I2S_BCLK_GPIO / WS / DIN` to match your wiring.
   - Optional `KWS_LED_GPIO` to flash an LED on detection.
2. From an ESP-IDF terminal:

```bash
cd firmware/hello_tors_kws
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor
```

(Replace `COM5` with your board's port. The first build downloads esp-tflite-micro.)

INMP441 wiring for the default pins:

| INMP441 | ESP32-S3 |
|---|---|
| VDD | 3V3 |
| GND | GND |
| L/R | GND (left slot) |
| SCK | GPIO 5 |
| WS | GPIO 4 |
| SD | GPIO 6 |

## 3. First boot — what you should see

```
kws: model: 36744 bytes, arena used NNNNN / 131072, input scale 0.558302 zp 93
kws: self-test keyword   features 1960 exact / 0 off-by-1 / 0 worse | P(kw) 0.977 raw 122 (PC 122) | ...
kws: self-test near-miss features 1960 exact / 0 off-by-1 / 0 worse | P(kw) 0.137 raw -93 (PC -93) | ...
kws: SELF-TEST PASS - device matches the PC reference
kws: timing: MFCC frame NNN us, invoke NNNN us, hop 20000 us -> inference every 1 hop(s)
kws: listening: threshold 0.50, smooth 2, gain 0.0 dB, noise floor off - say "Hello Tors"
kws: mic: peak -xx.x dBFS (after gain -xx.x), quietest -xx.x dBFS | max P(kw) 0.00
kws: >>> HELLO TORS  P=0.61  t=12.34 s
```

- **SELF-TEST PASS** proves the on-device features and TFLite Micro output match the PC
  bit-for-bit (small off-by-1 counts are tolerated). If it fails, detections won't match
  the PC results — fix that before tuning anything.
- **AllocateTensors failed** → raise `KWS_TENSOR_ARENA_SIZE`.
- The model was exported from Keras with a dynamic batch dimension, so it starts with
  `SHAPE → STRIDED_SLICE → PACK → RESHAPE`. TFLite Micro supports these (they're registered
  in `app_main.cc`) and uses the static `[1,49,40]` shape. If you ever see an op-not-found
  or reshape error, re-export the model with a fixed batch size of 1.
- If inference is slower than 20 ms, the firmware automatically runs it every N hops and
  logs a warning. Detection still works; the smoothing window just spans more audio.

## 4. Calibrating the mic level (important)

The model is **level-sensitive**. Found on the PC with the Realme Buds:

| Buds audio, as recorded | Result |
|---|---|
| raw (speech ≈ −10 dBFS, digital-silence pauses) | P(kw) ≤ 0.09, 0 of 5 detected |
| −25 dB gain only | peak 0.36, 0 of 5 |
| −25 dB gain + −55 dBFS pink-noise floor | peak 0.97, 5 of 5 at threshold 0.5, 0 false |

Training data: keyword ≈ −30 dBFS (100 ms RMS peak), room floor ≈ −50 dBFS.

Procedure on the hardware (`KWS_LOG_LEVELS 1`):
1. Say "Hello Tors" at your normal distance and read the `mic: peak` log line.
2. Set `KWS_INPUT_GAIN_DB` so **peak after gain ≈ −30 dBFS** (e.g. peak −48 → gain +18).
   An INMP441 at arm's length is typically quiet (−45 to −60 dBFS), so it will usually need
   positive gain. Audio much louder than −20 dBFS makes the model miss.
3. Leave `KWS_NOISE_FLOOR_DB` off unless the `quietest` level reads below about −90 dBFS
   (digital silence). Real MEMS mics have their own noise floor.

You can check a setting offline first: record through the same mic, then
`python scan_audio.py rec.wav --gain-db G --noise-floor-db F --threshold 0.5 --smooth 2`.

## 5. Detection settings

`DETECTION_THRESHOLD 0.5`, `SMOOTHING_WINDOW 2`, `REFRACTORY_MS 1000` (in `kws_config.h`).
The training report used 0.85. On the PC, 0.5 + 2 consecutive windows gave:

| Test | Result |
|---|---|
| `Naman_Close_clean.wav` (10 keywords) | 10/10, 0 false |
| Live Buds recording (5 keywords), conditioned | 5/5, 0 false |
| 0.85 threshold on the same audio | 7/10 and 2–3/5 |

These numbers come from two speakers. Check false triggers on the hardware during normal
conversation before trusting them. `SMOOTHING_WINDOW 3` or a higher threshold trades
missed keywords for fewer false triggers.

## 6. Verifying changes on the PC (no hardware needed)

With the Python venv active (`pip install -r requirements.txt`) and a gcc on PATH:

```bash
python tools/verify_c_features.py test_audio/Naman_Close_clean.wav
```

Verification results for this package:

- **Self-test clips:** 1960/1960 int8 inputs identical to TensorFlow.
- **`Naman_Close_clean.wav`:** 2384 streamed windows, 100% identical, and the same 10 detections as Python.
- **`buds_test2.wav`:** 99.99% identical (never off by more than 1); P(keyword) identical to 4 decimals.

If you retrain the model or change `feature_config.json`, re-run
`python tools/gen_firmware_tables.py` (it re-derives the tables from TensorFlow, rebuilds the
self-test vectors and checks the model header against the `.tflite`), and regenerate
`hello_tors_model_data.h`. The `static_assert`s in `app_main.cc` stop a mismatched build.

## 7. Known limits

- The firmware was written against the ESP-IDF 5.3 and esp-tflite-micro APIs and
  syntax-checked on the PC, but it has **not been compiled with ESP-IDF or run on a board**.
  The first `idf.py build` and the boot self-test are the real test.
- Model accuracy is modest. The training report gives TPR 43% at 0.85 on held-out
  speakers. Retraining with level augmentation (±20 dB) and noise-gated/silent-pause
  augmentation would remove the need for input conditioning.
