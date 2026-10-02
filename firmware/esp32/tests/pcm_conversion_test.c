#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "audio_conversion.h"

int main(void) {
    const int32_t input[] = {0x00010000, 0x7fff0000, (int32_t)0xffff0000, 0x00007fff};
    int16_t output[4] = {0};

    convert_i2s_to_pcm(input, output, 4);

    assert(output[0] == 1);
    assert(output[1] == 32767);
    assert(output[2] == -1);
    assert(output[3] == 0);

    assert(speaker_pcm_to_i2s(0) == 0);
    assert(speaker_pcm_to_i2s(10000) == 7000 * 65536);
    assert(speaker_pcm_to_i2s(-10000) == -7000 * 65536);
    assert(speaker_pcm_to_i2s(INT16_MAX) == 22936 * 65536);
    assert(speaker_pcm_to_i2s(INT16_MIN) == -22937 * 65536);
    for (int32_t sample = INT16_MIN; sample <= INT16_MAX; ++sample) {
        const int64_t expected = ((int64_t)sample * 70 / 100) * 65536;
        assert(speaker_pcm_to_i2s((int16_t)sample) == expected);
    }
    puts("pcm conversion and 70% speaker gain tests passed (all 65536 PCM16 values)");
    return 0;
}
