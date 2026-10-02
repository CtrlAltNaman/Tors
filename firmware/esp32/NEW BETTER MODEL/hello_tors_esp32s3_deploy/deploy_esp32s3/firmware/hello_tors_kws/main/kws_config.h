// Hello/Tors KWS firmware configuration - edit this file for your board and mic.
#pragma once

// ---------------------------------------------------------------------------------
// Microphone
// ---------------------------------------------------------------------------------
#define KWS_MIC_TYPE_I2S_STD 0  // I2S MEMS mic: INMP441, ICS-43434, SPH0645, MSM261...
#define KWS_MIC_TYPE_PDM     1  // PDM mic, e.g. Seeed XIAO ESP32S3 Sense on-board mic
#define KWS_MIC_TYPE KWS_MIC_TYPE_I2S_STD

// I2S (standard/Philips) pins - SET THESE TO YOUR WIRING.
// INMP441: SCK -> BCLK, WS -> WS, SD -> DIN, L/R -> GND (left slot), VDD 3V3.
#define KWS_I2S_BCLK_GPIO 5
#define KWS_I2S_WS_GPIO   4
#define KWS_I2S_DIN_GPIO  6
#define KWS_I2S_USE_RIGHT_SLOT 0  // 1 if the mic's L/R (SEL) pin is tied to VDD

// PDM pins (defaults = XIAO ESP32S3 Sense: CLK GPIO42, DATA GPIO41).
#define KWS_PDM_CLK_GPIO 42
#define KWS_PDM_DIN_GPIO 41

// ---------------------------------------------------------------------------------
// Input conditioning (see kws_audio.h and README "Calibrating the mic level")
// ---------------------------------------------------------------------------------
// The model was trained on speech at ~-30 dBFS (100 ms RMS peaks while saying the
// keyword) over a ~-50 dBFS room-noise floor. Speech much louder than that scores ~0.
// Watch the "mic:" log lines (KWS_LOG_LEVELS) while speaking at your normal distance
// and set the gain so "peak after gain" lands around -30 dBFS.
#define KWS_INPUT_GAIN_DB   0.0f
// Adds a pink-noise room floor. Only needed if the mic delivers digital silence in pauses
// (noise-gated headsets did: -25 dB gain + -55 dBFS floor fixed them on the PC).
// <= -120 disables it. Real I2S/PDM MEMS mics have their own noise floor.
#define KWS_NOISE_FLOOR_DB  (-200.0f)
#define KWS_DC_BLOCK        1  // remove mic DC offset (SPH0645 has a large one)

// ---------------------------------------------------------------------------------
// Detection (validated on the PC: 0.5 + 2 consecutive windows caught 10/10 on
// Naman_Close_clean.wav and 5/5 live, with 0 false triggers)
// ---------------------------------------------------------------------------------
#define DETECTION_THRESHOLD   0.5f
#define SMOOTHING_WINDOW      2     // consecutive inferences >= threshold to fire
#define REFRACTORY_MS         1000  // ignore further detections for this long
#define KWS_KEYWORD_INDEX     1     // model output index of P("Hello Tors")
// Run the model every N hops (1 hop = 20 ms). Raised automatically at boot if the
// measured inference time does not fit (SMOOTHING_WINDOW then spans more audio).
#define KWS_INFERENCE_EVERY_HOPS 1
// No detections until the 1 s buffer holds only real audio (it starts as digital silence,
// and the jump from silence to mic noise can look like an onset).
#define KWS_WARMUP_HOPS 49

// ---------------------------------------------------------------------------------
// Output / debug
// ---------------------------------------------------------------------------------
#define KWS_LED_GPIO          (-1)  // GPIO to pulse on detection, -1 = none (XIAO S3: 21)
#define KWS_LED_ACTIVE_LEVEL  1     // XIAO S3 user LED is active-low: 0
#define KWS_LED_ON_MS         500
#define KWS_LOG_LEVELS        1     // log mic level + max P(kw) once per second
#define KWS_RUN_SELF_TEST     1     // verify features + model against the PC at boot

#define KWS_TENSOR_ARENA_SIZE (128 * 1024)
