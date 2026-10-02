#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mic_protocol.h"

int main(void) {
    assert(mic_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u);
    assert(mic_crc32((const uint8_t *)"", 0) == 0);
    int32_t words[MIC_FRAME_SAMPLES] = {0};
    words[0] = INT32_MIN;
    words[1] = INT32_MAX;
    words[2] = -65536;
    uint8_t frame[MIC_PACKET_BYTES];
    mic_encode(frame, 0x12345678, words);
    assert(memcmp(frame, "MIC1\x78\x56\x34\x12\x80\x02\x80\x3e", 12) == 0);
    assert(memcmp(frame + 12, "\x00\x80\xff\x7f\xff\xff\x00\x00", 8) == 0);
    for (unsigned i = 0; i < sizeof(frame); ++i) printf("%02x", frame[i]);
    puts("");
    return 0;
}
