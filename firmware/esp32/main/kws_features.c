#include "kws_features.h"

#include <math.h>
#include <string.h>

#include "kws_feature_tables.h"

// The generated tables must describe the same pipeline this file implements.
_Static_assert(KWS_TBL_SAMPLE_RATE == KWS_SAMPLE_RATE, "sample rate mismatch");
_Static_assert(KWS_TBL_FRAME_LEN == KWS_FRAME_LEN, "frame length mismatch");
_Static_assert(KWS_TBL_FRAME_STEP == KWS_FRAME_STEP, "frame step mismatch");
_Static_assert(KWS_TBL_FFT_LEN == KWS_FFT_LEN, "FFT length mismatch");
_Static_assert(KWS_TBL_NUM_MFCC == KWS_NUM_MFCC, "MFCC count mismatch");
_Static_assert(KWS_TBL_NUM_MEL == KWS_NUM_MFCC, "mel count mismatch");
_Static_assert(KWS_TBL_NUM_FRAMES == KWS_NUM_FRAMES, "frame count mismatch");

#define NUM_BINS (KWS_FFT_LEN / 2 + 1)
#define LOG2_FFT 9
_Static_assert((1 << LOG2_FFT) == KWS_FFT_LEN, "FFT length must be 2^LOG2_FFT");

static float s_cos[KWS_FFT_LEN / 2];
static float s_sin[KWS_FFT_LEN / 2];
static uint16_t s_bitrev[KWS_FFT_LEN];

void kws_features_init(void) {
    for (int i = 0; i < KWS_FFT_LEN / 2; i++) {
        double a = -2.0 * 3.14159265358979323846 * i / KWS_FFT_LEN;
        s_cos[i] = (float)cos(a);
        s_sin[i] = (float)sin(a);
    }
    for (int i = 0; i < KWS_FFT_LEN; i++) {
        int r = 0;
        for (int b = 0; b < LOG2_FFT; b++) r |= ((i >> b) & 1) << (LOG2_FFT - 1 - b);
        s_bitrev[i] = (uint16_t)r;
    }
}

// In-place iterative radix-2 complex FFT (forward, unnormalised).
static void fft512(float *re, float *im) {
    for (int i = 0; i < KWS_FFT_LEN; i++) {
        int j = s_bitrev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= KWS_FFT_LEN; len <<= 1) {
        int half = len >> 1, step = KWS_FFT_LEN / len;
        for (int i = 0; i < KWS_FFT_LEN; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = s_cos[k * step], wi = s_sin[k * step];
                int a = i + k, b = a + half;
                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
}

void kws_mfcc_frame(const float *frame, float *mfcc) {
    // Single owner: boot self-test, then the inference task. Avoid 5284 B on
    // either task's stack. Arithmetic and tables are unchanged from the package.
    static float re[KWS_FFT_LEN], im[KWS_FFT_LEN], mag[NUM_BINS], log_mel[KWS_NUM_MFCC];

    for (int n = 0; n < KWS_FRAME_LEN; n++) re[n] = frame[n] * kws_hann[n];
    for (int n = KWS_FRAME_LEN; n < KWS_FFT_LEN; n++) re[n] = 0.0f;  // tf.signal.stft pads at the end
    memset(im, 0, sizeof(im));
    fft512(re, im);
    for (int k = 0; k < NUM_BINS; k++) mag[k] = sqrtf(re[k] * re[k] + im[k] * im[k]);

    const float *w = kws_mel_weights;
    for (int m = 0; m < KWS_NUM_MFCC; m++) {
        float acc = 0.0f;
        const float *bins = mag + kws_mel_start[m];
        for (int j = 0; j < kws_mel_len[m]; j++) acc += w[j] * bins[j];
        w += kws_mel_len[m];
        log_mel[m] = logf(acc + KWS_TBL_LOG_EPSILON);
    }

    for (int k = 0; k < KWS_NUM_MFCC; k++) {
        float acc = 0.0f;
        for (int n = 0; n < KWS_NUM_MFCC; n++) acc += kws_dct[k][n] * log_mel[n];
        mfcc[k] = acc;
    }
}

void kws_mfcc_window(const float *audio, float *mfcc) {
    for (int f = 0; f < KWS_NUM_FRAMES; f++)
        kws_mfcc_frame(audio + f * KWS_FRAME_STEP, mfcc + f * KWS_NUM_MFCC);
}

int8_t kws_quantize(float v, float scale, int zero_point) {
    // nearbyintf rounds half to even under the default rounding mode, like numpy.round.
    float q = nearbyintf(v / scale + (float)zero_point);
    if (q < -128.0f) q = -128.0f;
    if (q > 127.0f) q = 127.0f;
    return (int8_t)q;
}

void kws_stream_reset(kws_stream_t *s) {
    memset(s->history, 0, sizeof(s->history));
    float silence[KWS_NUM_MFCC];
    kws_mfcc_frame(s->history, silence);
    for (int f = 0; f < KWS_NUM_FRAMES; f++) memcpy(s->mfcc[f], silence, sizeof(silence));
}

void kws_stream_push(kws_stream_t *s, const float *hop) {
    // The newest frame is the last KWS_FRAME_LEN samples: 160 old + 320 new.
    memmove(s->history, s->history + KWS_FRAME_STEP,
            (KWS_FRAME_LEN - KWS_FRAME_STEP) * sizeof(float));
    memcpy(s->history + (KWS_FRAME_LEN - KWS_FRAME_STEP), hop, KWS_FRAME_STEP * sizeof(float));
    memmove(s->mfcc[0], s->mfcc[1], (KWS_NUM_FRAMES - 1) * sizeof(s->mfcc[0]));
    kws_mfcc_frame(s->history, s->mfcc[KWS_NUM_FRAMES - 1]);
}

void kws_stream_quantize(const kws_stream_t *s, int8_t *out, float scale, int zero_point) {
    const float *m = &s->mfcc[0][0];
    for (int i = 0; i < KWS_NUM_FRAMES * KWS_NUM_MFCC; i++) out[i] = kws_quantize(m[i], scale, zero_point);
}
