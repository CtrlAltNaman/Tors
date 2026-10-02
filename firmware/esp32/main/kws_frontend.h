#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "kws_features.h"

#define KWS_INPUT_SCALE 0.5583019852638245f
#define KWS_INPUT_ZERO_POINT 93
// User-confirmed trial setting; supplied package default is 0.50.
#define KWS_DETECTION_THRESHOLD 0.450f
#define KWS_REQUIRED_POSITIVES 2
#define KWS_REFRACTORY_US 1000000

// Same features as the supplied float streaming cache, quantized once per row.
// No gain, DC blocker, noise injection, denoising, or signal conditioning.
typedef struct {
    float history[KWS_FRAME_LEN];
    int8_t features[KWS_NUM_FRAMES * KWS_NUM_MFCC];
    unsigned hops;
} kws_frontend_t;

typedef struct {
    unsigned consecutive;
    bool fired;
    int64_t last_fire_us;
} kws_decision_t;

#ifdef __cplusplus
extern "C" {
#endif
void kws_frontend_reset(kws_frontend_t *s);
bool kws_frontend_push(kws_frontend_t *s, const int16_t *pcm);
bool kws_decision_update(kws_decision_t *d, float score, int64_t now_us);
// Returns 0 if feature processing alone cannot keep up. Includes 25% timing
// margin and one RTOS yield per invocation. It is a throughput budget, NOT a
// claim that CPU utilization is under 10%.
unsigned kws_inference_stride(uint32_t invoke_us, uint32_t feature_us, uint32_t yield_us);
#ifdef __cplusplus
}
#endif
