#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define REC_META_BYTES 40
#define REC_SLOT_BYTES (1024u*1024u)
#define REC_DATA_OFFSET 4096u
#define REC_SLOT_COUNT 4
#define REC_KEEP_COUNT 3
#define REC_MAX_PCM_BYTES (30u*16000u*2u)
typedef enum { REC_SILENCE=0, REC_LIMIT, REC_GAP, REC_IO_ERROR } recording_reason_t;
typedef struct {
    uint32_t id, stream_id, pcm_bytes, pcm_crc;
    recording_reason_t reason;
} recording_info_t;
uint32_t recording_crc32(uint32_t crc, const void *bytes, size_t count);
void recording_wav_header(uint8_t out[44], uint32_t pcm_bytes);
void recording_encode(uint8_t out[REC_META_BYTES], const recording_info_t *info);
bool recording_decode(const uint8_t in[REC_META_BYTES], recording_info_t *info);
bool recording_parse_path(const char *path, uint32_t *id);
bool recording_parse_range(const char *value, uint32_t size, uint32_t *first, uint32_t *last);
const char *recording_reason_name(recording_reason_t reason);
