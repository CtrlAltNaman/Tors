#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "stream_policy.h"

int main(void) {
    assert(kws_probability(-128, 1.0f / 256, -128) == 0.0f);
    assert(fabsf(kws_probability(127, 1.0f / 256, -128) - 255.0f / 256) < 1e-6f);

    int16_t pcm[AUDIO_FRAME_SAMPLES];
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; ++i) pcm[i] = 1000;
    assert(audio_ac_rms(pcm) == 0.0f); /* DC is not speech. */
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; ++i) pcm[i] += (i & 1) ? 100 : -100;
    assert(fabsf(audio_ac_rms(pcm) - 100.0f) < 0.01f);
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; ++i) pcm[i] = (i & 1) ? INT16_MAX : INT16_MIN;
    assert(fabsf(audio_ac_rms(pcm) - 32767.5f) < 0.01f); /* No accumulator overflow. */

    ambient_tracker_t ambient = {0};
    for (int i = 0; i < AMBIENT_CALIBRATION_FRAMES; ++i) ambient_observe(&ambient, 40);
    assert(ambient.ready);
    for (int i = 0; i < 100; ++i) ambient_observe(&ambient, 3000);
    assert(ambient.noise_rms < 50); /* Do not learn speech as background. */

    voice_endpoint_t endpoint;
    voice_endpoint_begin(&endpoint, ambient.noise_rms);
    for (int i = 0; i < 1500; ++i) assert(!voice_endpoint_observe(&endpoint, 2000));
    for (int i = 0; i < 50; ++i) assert(!voice_endpoint_observe(&endpoint, 40));
    assert(!voice_endpoint_observe(&endpoint, 2000)); /* A short pause resets. */
    for (int i = 1; i < ENDPOINT_SILENCE_FRAMES; ++i)
        assert(!voice_endpoint_observe(&endpoint, 40));
    assert(voice_endpoint_observe(&endpoint, 40));
    voice_endpoint_begin(&endpoint, 40);
    for (int i = 1; i < ENDPOINT_MIN_FRAMES; ++i)
        assert(!voice_endpoint_observe(&endpoint, 40));
    assert(voice_endpoint_observe(&endpoint, 40));

    static audio_history_t history;
    for (int n = 0; n < AUDIO_HISTORY_FRAMES + 8; ++n) {
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; ++i) pcm[i] = (int16_t)n;
        audio_history_push(&history, pcm);
    }
    assert(!audio_history_copy(&history, 7, pcm)); /* Never silently send overwritten data. */
    for (int n = 8; n < AUDIO_HISTORY_FRAMES + 8; ++n) {
        assert(audio_history_copy(&history, n, pcm));
        assert(pcm[0] == n && pcm[AUDIO_FRAME_SAMPLES - 1] == n);
    }
    assert(!audio_history_copy(&history, history.total_frames, pcm));
    puts("stream policy: probability, ambient calibration, endpoint, long speech and ring wrap passed");
    return 0;
}
