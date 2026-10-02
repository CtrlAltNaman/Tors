#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kws_frontend.h"
#include "kws_test_vectors.h"

static void vector_test(const int16_t *pcm, const int8_t *expected) {
    float frame[KWS_FRAME_LEN], mfcc[KWS_NUM_MFCC];
    int mismatches = 0;
    for (int f = 0; f < KWS_NUM_FRAMES; ++f) {
        for (int i = 0; i < KWS_FRAME_LEN; ++i)
            frame[i] = pcm[f * KWS_FRAME_STEP + i] / 32768.0f;
        kws_mfcc_frame(frame, mfcc);
        for (int m = 0; m < KWS_NUM_MFCC; ++m)
            mismatches += kws_quantize(mfcc[m], KWS_INPUT_SCALE, KWS_INPUT_ZERO_POINT)
                          != expected[f * KWS_NUM_MFCC + m];
    }
    printf("Golden vector: %d/1960 exact\n", 1960 - mismatches);
    assert(mismatches == 0);
}

static void stream_test(void) {
    kws_frontend_t compact;
    kws_stream_t reference;
    float hop[KWS_FRAME_STEP];
    int16_t pcm[KWS_FRAME_STEP];
    int8_t expected[KWS_NUM_FRAMES * KWS_NUM_MFCC];
    kws_frontend_reset(&compact);
    kws_stream_reset(&reference);
    for (int n = 0; n < 150; ++n) {
        for (int i = 0; i < KWS_FRAME_STEP; ++i) {
            pcm[i] = kws_test_pos_pcm[(n * KWS_FRAME_STEP + i) % KWS_WINDOW_SAMPLES];
            hop[i] = pcm[i] / 32768.0f;
        }
        bool ready = kws_frontend_push(&compact, pcm);
        kws_stream_push(&reference, hop);
        kws_stream_quantize(&reference, expected, KWS_INPUT_SCALE, KWS_INPUT_ZERO_POINT);
        assert(memcmp(compact.features, expected, sizeof(expected)) == 0);
        assert(ready == (n >= 49));
    }
    kws_frontend_reset(&compact);
    assert(compact.hops == 0);
    assert(!kws_frontend_push(&compact, pcm));
}

static void decision_test(void) {
    kws_decision_t d = {0};
    assert(!kws_decision_update(&d, 0.45f, 0));
    assert(!kws_decision_update(&d, 0.449f, 200000));
    assert(!kws_decision_update(&d, 0.75f, 400000)); // two-of-three must NOT fire
    assert(kws_decision_update(&d, 0.45f, 600000));
    assert(!kws_decision_update(&d, 0.99f, 1599999));
    assert(kws_decision_update(&d, 0.99f, 1600000));
    assert(!kws_decision_update(&d, NAN, 2600000));
    assert(!kws_decision_update(&d, 0.99f, 2800000));
    assert(kws_decision_update(&d, 0.99f, 3000000));
    // Real int8 outputs straddle 0.45 at raw -13 and -12 (scale 1/256, zp -128).
    kws_decision_t quantized = {0};
    assert(!kws_decision_update(&quantized, 115.0f / 256, 0));
    assert(!kws_decision_update(&quantized, 116.0f / 256, 200000));
    assert(kws_decision_update(&quantized, 116.0f / 256, 400000));
    assert(kws_inference_stride(120000, 1000, 10000) >= 9);
    assert(kws_inference_stride(1000, 100, 1000) == 1);
    assert(kws_inference_stride(120000, 20000, 10000) == 0);
}

int main(void) {
    kws_features_init();
    vector_test(kws_test_pos_pcm, kws_test_pos_features);
    vector_test(kws_test_neg_pcm, kws_test_neg_features);
    assert(kws_quantize(0.5f, 1.0f, 93) == 94);
    assert(kws_quantize(1.5f, 1.0f, 93) == 94);
    assert(kws_quantize(-1000, 1, 0) == -128);
    assert(kws_quantize(1000, 1, 0) == 127);
    stream_test();
    decision_test();
    puts("KWS frontend, streaming parity, quantization and decision tests passed");
    return 0;
}
