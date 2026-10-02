#include "kws_audio.h"

#include <math.h>
#include <string.h>

// Paul Kellet's pink-noise filter (-3 dB/octave), same coefficients as scan_audio.py.
static const float B[4] = {0.049922035f, -0.095993537f, 0.050612699f, -0.004408786f};
static const float A[4] = {1.0f, -2.494956002f, 2.017265875f, -0.522189400f};

// Uniform white noise in [-1, 1) from xorshift32. Its spectrum is flat like the Gaussian
// noise in the Python version; the RMS difference is removed by the calibration below.
static float white(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

// Transposed direct form II.
static float pink(kws_conditioner_t *c) {
    float w = white(&c->rng);
    float y = B[0] * w + c->z[0];
    c->z[0] = B[1] * w - A[1] * y + c->z[1];
    c->z[1] = B[2] * w - A[2] * y + c->z[2];
    c->z[2] = B[3] * w - A[3] * y;
    return y;
}

void kws_conditioner_init(kws_conditioner_t *c, float gain_db, float noise_floor_db, int dc_block) {
    memset(c, 0, sizeof(*c));
    c->gain = powf(10.0f, gain_db / 20.0f);
    c->dc_block = dc_block;
    c->rng = 0x9E3779B9u;
    if (noise_floor_db > -120.0f) {
        // Calibrate so the pink noise RMS equals noise_floor_db (skip the filter warm-up).
        double acc = 0.0;
        const int warm = 16000, n = 5 * 16000;
        for (int i = 0; i < warm + n; i++) {
            float y = pink(c);
            if (i >= warm) acc += (double)y * y;
        }
        c->noise_scale = powf(10.0f, noise_floor_db / 20.0f) / (float)sqrt(acc / n);
    }
}

void kws_conditioner_process(kws_conditioner_t *c, float *x, int n) {
    for (int i = 0; i < n; i++) {
        float v = x[i];
        if (c->dc_block) {
            // One-pole DC blocker, corner ~8 Hz at 16 kHz (below the 20 Hz mel floor).
            float y = v - c->dc_x1 + 0.997f * c->dc_y1;
            c->dc_x1 = v;
            c->dc_y1 = y;
            v = y;
        }
        v *= c->gain;
        if (c->noise_scale > 0.0f) v += pink(c) * c->noise_scale;
        x[i] = v;
    }
}
