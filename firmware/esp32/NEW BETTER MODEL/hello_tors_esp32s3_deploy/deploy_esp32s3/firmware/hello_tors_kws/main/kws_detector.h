// Detection logic - port of scan_audio.Detector: fire when `smooth` consecutive
// inferences score >= threshold, then ignore detections for `refractory` hops.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float threshold;
    int smooth;
    int refractory_hops;
    int run;
    int hops_since_fire;
} kws_detector_t;

void kws_detector_init(kws_detector_t *d, float threshold, int smooth, int refractory_hops);
// p: P(keyword) of the newest window; hops: audio hops since the previous update.
bool kws_detector_update(kws_detector_t *d, float p, int hops);

#ifdef __cplusplus
}
#endif
