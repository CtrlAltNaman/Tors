// Input conditioning: make the mic signal look like the training recordings before
// feature extraction. Port of scan_audio.Conditioner, plus an optional DC blocker.
//
// The model is level-sensitive: speech much louder than the training data (keyword
// ~-34 dBFS RMS, 100 ms peaks ~-30 dBFS) scores near 0, and so does speech whose
// pauses are digital silence (headset noise gates) - the training data always had a
// room-noise floor (~-50 dBFS). Calibrate KWS_INPUT_GAIN_DB / KWS_NOISE_FLOOR_DB
// with KWS_LOG_LEVELS; see README.md.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float gain;
    int dc_block;
    float dc_x1, dc_y1;
    float noise_scale;  // 0 = no noise floor
    float z[3];         // pink filter state
    uint32_t rng;
} kws_conditioner_t;

// noise_floor_db <= -120 disables the added noise floor.
void kws_conditioner_init(kws_conditioner_t *c, float gain_db, float noise_floor_db, int dc_block);
// In place: DC block (optional) -> gain -> + pink noise floor (optional).
void kws_conditioner_process(kws_conditioner_t *c, float *x, int n);

#ifdef __cplusplus
}
#endif
