#include "stream_policy.h"
#include <math.h>
#include <string.h>

float kws_probability(int8_t value, float scale, int zero_point) {
    /* Exported model already ends in Softmax. Only dequantize its output. */
    return ((float)value - zero_point) * scale;
}

float audio_ac_rms(const int16_t *samples) {
    int64_t sum = 0, square_sum = 0;
    for (unsigned i = 0; i < AUDIO_FRAME_SAMPLES; ++i) {
        const int32_t value = samples[i];
        sum += value;
        square_sum += (int64_t)value * value;
    }
    const double mean = (double)sum / AUDIO_FRAME_SAMPLES;
    const double variance = (double)square_sum / AUDIO_FRAME_SAMPLES - mean * mean;
    return sqrtf((float)(variance > 0 ? variance : 0));
}

void ambient_observe(ambient_tracker_t *state, float rms) {
    if (!state->ready) {
        state->calibration_frames++;
        state->noise_rms += (rms - state->noise_rms) / state->calibration_frames;
        state->ready = state->calibration_frames >= AMBIENT_CALIBRATION_FRAMES;
    } else if (rms <= fmaxf(VOICE_RMS_MIN, state->noise_rms * 1.5f)) {
        /* Track slow background changes while idle; do not absorb loud speech. */
        state->noise_rms += 0.01f * (rms - state->noise_rms);
    }
}

void voice_endpoint_begin(voice_endpoint_t *state, float noise_rms) {
    *state = (voice_endpoint_t){
        .threshold_rms = fmaxf(VOICE_RMS_MIN, noise_rms * VOICE_NOISE_RATIO),
    };
}

bool voice_endpoint_observe(voice_endpoint_t *state, float rms) {
    if (state->elapsed_frames < ENDPOINT_MIN_FRAMES) state->elapsed_frames++;
    if (rms > state->threshold_rms) state->quiet_frames = 0;
    else if (state->quiet_frames < ENDPOINT_SILENCE_FRAMES) state->quiet_frames++;
    return state->elapsed_frames >= ENDPOINT_MIN_FRAMES &&
           state->quiet_frames >= ENDPOINT_SILENCE_FRAMES;
}

void audio_history_push(audio_history_t *history, const int16_t *samples) {
    memcpy(history->frames[history->total_frames % AUDIO_HISTORY_FRAMES],
           samples, sizeof(history->frames[0]));
    history->total_frames++;
}

bool audio_history_copy(const audio_history_t *history, uint64_t index, int16_t *samples) {
    if (index >= history->total_frames || history->total_frames - index > AUDIO_HISTORY_FRAMES)
        return false;
    memcpy(samples, history->frames[index % AUDIO_HISTORY_FRAMES], sizeof(history->frames[0]));
    return true;
}
