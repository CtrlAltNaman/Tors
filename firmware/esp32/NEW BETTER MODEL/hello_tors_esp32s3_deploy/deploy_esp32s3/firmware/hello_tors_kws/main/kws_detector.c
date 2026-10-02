#include "kws_detector.h"

void kws_detector_init(kws_detector_t *d, float threshold, int smooth, int refractory_hops) {
    d->threshold = threshold;
    d->smooth = smooth;
    d->refractory_hops = refractory_hops;
    d->run = 0;
    d->hops_since_fire = 1 << 30;
}

bool kws_detector_update(kws_detector_t *d, float p, int hops) {
    if (d->hops_since_fire < (1 << 30)) d->hops_since_fire += hops;
    d->run = p >= d->threshold ? d->run + 1 : 0;
    if (d->run >= d->smooth && d->hops_since_fire >= d->refractory_hops) {
        d->hops_since_fire = 0;
        return true;
    }
    return false;
}
