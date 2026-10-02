#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

#define PLAYBACK_QUEUE_FRAMES 64
#define PLAYBACK_PREBUFFER_FRAMES 5
#define PLAYBACK_PREBUFFER_WAIT_MS 250
#define PLAYBACK_FRAME_SAMPLES 320
#define PLAYBACK_WIRE_BYTES 656

typedef struct {
    int16_t samples[PLAYBACK_FRAME_SAMPLES];
    uint32_t audio_id, generation;
    bool last;
} playback_frame_t;
typedef struct {
    uint32_t received, submitted, overflows, invalid, write_errors, underruns;
    uint32_t rx_gap_max_ms, write_max_ms;
    unsigned queued, high_water;
} playback_stats_t;

bool playback_buffer_init(void);
/* Receive-side owner: the WebSocket event task. Reset invalidates in-flight reads. */
void playback_buffer_begin(void);
void playback_buffer_end(void);
void playback_buffer_reset(void);
void playback_buffer_receive(const void *data, size_t size, size_t total, size_t offset);
bool playback_buffer_read(playback_frame_t *frame, TickType_t wait);
/* Startup/restart only. held_frames are current-generation frames owned by TX. */
bool playback_buffer_ready(uint32_t generation, unsigned held_frames);
bool playback_buffer_current(uint32_t generation);
bool playback_buffer_ended(void);
/* End published and software queue empty for this generation. The TX owner
 * must also allow its held frame and DMA tail to finish before acknowledging. */
bool playback_buffer_drained(uint32_t generation);
bool playback_buffer_failed(uint32_t generation);
void playback_buffer_written(uint32_t generation, bool success);
void playback_buffer_write_timing(uint32_t duration_ms);
void playback_buffer_underrun(void);
void playback_buffer_get_stats(playback_stats_t *stats);
