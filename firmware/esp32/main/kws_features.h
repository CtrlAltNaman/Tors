// Hello/Tors KWS feature front end - portable C, no ESP-IDF dependencies.
//
// Bit-for-bit port of the training pipeline (TensorFlow tf.signal):
//   frame 480 samples, step 320, periodic Hann, zero-pad to 512, |rfft| (magnitude)
//   -> 40-band HTK mel (20-8000 Hz) -> log(mel + 1e-6) -> mfccs_from_log_mel_spectrograms
//   -> 49 frames x 40 coefficients per 1 s window.
// Tables come from tools/gen_firmware_tables.py; tools/verify_c_features.py checks this
// file against the Python/TensorFlow reference on a PC.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KWS_SAMPLE_RATE    16000
#define KWS_FRAME_LEN      480
#define KWS_FRAME_STEP     320
#define KWS_FFT_LEN        512
#define KWS_NUM_MFCC       40
#define KWS_NUM_FRAMES     49
#define KWS_WINDOW_SAMPLES ((KWS_NUM_FRAMES - 1) * KWS_FRAME_STEP + KWS_FRAME_LEN)  // 15840

// Must be called once before anything else (builds FFT twiddles).
void kws_features_init(void);

// One frame: KWS_FRAME_LEN samples in [-1, 1) -> KWS_NUM_MFCC coefficients.
// Shared scratch workspace: NOT reentrant; boot/inference task are sole owners.
void kws_mfcc_frame(const float *frame, float *mfcc);

// A whole 1 s window: KWS_WINDOW_SAMPLES samples -> [KWS_NUM_FRAMES][KWS_NUM_MFCC].
void kws_mfcc_window(const float *audio, float *mfcc);

// round-half-even(v / scale + zero_point), clamped to int8 - same as numpy.round.
int8_t kws_quantize(float v, float scale, int zero_point);

// Streaming: push KWS_FRAME_STEP new samples at a time; the object always holds the
// features of the most recent 1 s, identical to recomputing kws_mfcc_window on it.
typedef struct {
    float history[KWS_FRAME_LEN];              // last 480 samples
    float mfcc[KWS_NUM_FRAMES][KWS_NUM_MFCC];  // oldest frame first
} kws_stream_t;

// Starts as 1 s of digital silence (matches the Python live path's zero-filled buffer).
void kws_stream_reset(kws_stream_t *s);
void kws_stream_push(kws_stream_t *s, const float *hop);
void kws_stream_quantize(const kws_stream_t *s, int8_t *out, float scale, int zero_point);

#ifdef __cplusplus
}
#endif
