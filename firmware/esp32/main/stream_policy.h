#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AUDIO_FRAME_SAMPLES 320
#define AUDIO_FRAME_MS 20
#define AUDIO_HISTORY_FRAMES 60
#define AMBIENT_CALIBRATION_FRAMES 100
#define ENDPOINT_SILENCE_FRAMES 75
#define ENDPOINT_MIN_FRAMES 100
#define VOICE_RMS_MIN 128.0f
#define VOICE_NOISE_RATIO 3.0f

typedef struct {
    int16_t frames[AUDIO_HISTORY_FRAMES][AUDIO_FRAME_SAMPLES];
    uint64_t total_frames;
} audio_history_t;

typedef struct {
    float noise_rms;
    unsigned calibration_frames;
    bool ready;
} ambient_tracker_t;

typedef struct {
    float threshold_rms;
    unsigned elapsed_frames;
    unsigned quiet_frames;
} voice_endpoint_t;

#ifdef __cplusplus
extern "C" {
#endif
float kws_probability(int8_t value, float scale, int zero_point);
float audio_ac_rms(const int16_t *samples);
void ambient_observe(ambient_tracker_t *state, float rms);
void voice_endpoint_begin(voice_endpoint_t *state, float noise_rms);
bool voice_endpoint_observe(voice_endpoint_t *state, float rms);
/* Caller serializes history access; these routines never block or allocate. */
void audio_history_push(audio_history_t *history, const int16_t *samples);
bool audio_history_copy(const audio_history_t *history, uint64_t index, int16_t *samples);
#ifdef __cplusplus
}
#endif
