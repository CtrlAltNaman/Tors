#pragma once

#include <stddef.h>
#include <stdint.h>

#define SPEAKER_VOLUME_PERCENT 70

void convert_i2s_to_pcm(const int32_t *input, int16_t *output, size_t sample_count);

// Speaker-only linear gain, then align PCM16 in the 32-bit I2S slot.
int32_t speaker_pcm_to_i2s(int16_t sample);
