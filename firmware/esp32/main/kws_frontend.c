#include "kws_frontend.h"
#include <string.h>

void kws_frontend_reset(kws_frontend_t *s) {
    memset(s, 0, sizeof(*s));
    float mfcc[KWS_NUM_MFCC];
    kws_mfcc_frame(s->history, mfcc);
    for (int m = 0; m < KWS_NUM_MFCC; ++m)
        s->features[m] = kws_quantize(mfcc[m], KWS_INPUT_SCALE, KWS_INPUT_ZERO_POINT);
    for (int f = 1; f < KWS_NUM_FRAMES; ++f)
        memcpy(s->features + f * KWS_NUM_MFCC, s->features, KWS_NUM_MFCC);
}

bool kws_frontend_push(kws_frontend_t *s, const int16_t *pcm) {
    memmove(s->history, s->history + KWS_FRAME_STEP,
            (KWS_FRAME_LEN - KWS_FRAME_STEP) * sizeof(float));
    for (int i = 0; i < KWS_FRAME_STEP; ++i)
        s->history[KWS_FRAME_LEN - KWS_FRAME_STEP + i] = pcm[i] / 32768.0f;
    memmove(s->features, s->features + KWS_NUM_MFCC,
            (KWS_NUM_FRAMES - 1) * KWS_NUM_MFCC);
    float mfcc[KWS_NUM_MFCC];
    kws_mfcc_frame(s->history, mfcc);
    for (int m = 0; m < KWS_NUM_MFCC; ++m)
        s->features[(KWS_NUM_FRAMES - 1) * KWS_NUM_MFCC + m] =
            kws_quantize(mfcc[m], KWS_INPUT_SCALE, KWS_INPUT_ZERO_POINT);
    // First frame contains 160 padded samples. Discard it before inference.
    if (s->hops < KWS_NUM_FRAMES + 1) ++s->hops;
    return s->hops > KWS_NUM_FRAMES;
}

bool kws_decision_update(kws_decision_t *d, float score, int64_t now_us) {
    if (score >= KWS_DETECTION_THRESHOLD) {
        if (d->consecutive < KWS_REQUIRED_POSITIVES) ++d->consecutive;
    } else {
        d->consecutive = 0;
    }
    if (d->consecutive < KWS_REQUIRED_POSITIVES ||
        (d->fired && now_us - d->last_fire_us < KWS_REFRACTORY_US)) return false;
    d->fired = true;
    d->last_fire_us = now_us;
    return true;
}

unsigned kws_inference_stride(uint32_t invoke_us, uint32_t feature_us, uint32_t yield_us) {
    const uint64_t feature_budget = ((uint64_t)feature_us * 5 + 3) / 4;
    if (feature_budget >= 20000) return 0;
    const uint64_t work = ((uint64_t)invoke_us * 5 + 3) / 4 + yield_us;
    const uint64_t available = 20000 - feature_budget;
    unsigned stride = (unsigned)((work + available - 1) / available);
    return stride ? stride : 1;
}
