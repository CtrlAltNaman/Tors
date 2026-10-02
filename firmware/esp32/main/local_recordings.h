#pragma once
#include "recording_format.h"
#include "esp_err.h"
typedef enum { REC_FRAME_WAIT, REC_FRAME_DATA, REC_FRAME_END, REC_FRAME_LOST } recording_frame_result_t;
typedef recording_frame_result_t (*recording_copy_fn)(uint32_t stream_id, uint64_t index, int16_t samples[320]);
typedef struct {
    bool enabled, active, preparing, ready;
    uint32_t saved, errors, pcm_bytes;
    const char *status;
} local_recordings_stats_t;
esp_err_t local_recordings_init(recording_copy_fn copy);
// No flash I/O or logging here; safe to call while the capture history is locked.
bool local_recordings_begin(uint32_t stream_id, uint64_t first_frame);
bool local_recordings_busy(void);
void local_recordings_allow_maintenance(bool allowed);
void local_recordings_get_stats(local_recordings_stats_t *out);
uint32_t local_recordings_stack_free(void);
unsigned local_recordings_list(recording_info_t out[REC_KEEP_COUNT]);
// A pinned slot is immutable until close; downloads may overlap recording.
int local_recordings_open(uint32_t id, recording_info_t *info);
esp_err_t local_recordings_read(int slot, uint32_t offset, void *data, size_t bytes);
void local_recordings_close(int slot);
