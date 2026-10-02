#include "mic_protocol.h"
#include <string.h>

uint32_t mic_crc32(const uint8_t *bytes, size_t length) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return crc ^ 0xffffffffu;
}

static void put16(uint8_t *output, uint16_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *output, uint32_t value) {
    put16(output, (uint16_t)value);
    put16(output + 2, (uint16_t)(value >> 16));
}

void mic_encode(uint8_t output[MIC_PACKET_BYTES], uint32_t sequence,
                const int32_t input[MIC_FRAME_SAMPLES]) {
    memcpy(output, "MIC1", 4);
    put32(output + 4, sequence);
    put16(output + 8, MIC_PCM_BYTES);
    put16(output + 10, MIC_SAMPLE_RATE);
    for (unsigned i = 0; i < MIC_FRAME_SAMPLES; ++i)
        put16(output + MIC_HEADER_BYTES + i * 2, (uint16_t)((uint32_t)input[i] >> 16));
    put32(output + MIC_PACKET_BYTES - 4, mic_crc32(output, MIC_PACKET_BYTES - 4));
}
