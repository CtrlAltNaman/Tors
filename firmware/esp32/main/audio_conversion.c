#include "audio_conversion.h"

_Static_assert(SPEAKER_VOLUME_PERCENT >= 0 && SPEAKER_VOLUME_PERCENT <= 100,
               "Speaker volume must be between 0 and 100 percent");

void convert_i2s_to_pcm(const int32_t *input, int16_t *output, size_t sample_count) {
    for (size_t i = 0; i < sample_count; ++i) {
        output[i] = (int16_t)(input[i] >> 16);
    }
}

int32_t speaker_pcm_to_i2s(int16_t sample) {
    // Scale before slot alignment: both multiplications fit in signed 32 bits.
    // Multiplication also avoids undefined left shifts of negative PCM samples.
    const int32_t scaled = (int32_t)sample * SPEAKER_VOLUME_PERCENT / 100;
    return scaled * 65536;
}
