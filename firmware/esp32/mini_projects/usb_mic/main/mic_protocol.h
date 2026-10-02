#pragma once
#include <stddef.h>
#include <stdint.h>

#define MIC_SAMPLE_RATE 16000
#define MIC_FRAME_SAMPLES 320
#define MIC_PCM_BYTES (MIC_FRAME_SAMPLES * 2)
#define MIC_HEADER_BYTES 12
#define MIC_PACKET_BYTES (MIC_HEADER_BYTES + MIC_PCM_BYTES + 4)

uint32_t mic_crc32(const uint8_t *bytes, size_t length);
void mic_encode(uint8_t output[MIC_PACKET_BYTES], uint32_t sequence,
                const int32_t input[MIC_FRAME_SAMPLES]);
