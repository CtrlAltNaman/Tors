#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "playback_buffer.h"

esp_err_t backend_client_init(void);
bool backend_client_is_ready(void);
bool backend_client_start_stream(uint32_t stream_id,
                                 uint32_t prebuffer_ms,
                                 uint32_t live_sample_offset,
                                 int64_t detection_time_ms);
bool backend_client_send_samples(const int16_t *samples, size_t sample_count,
                                 bool prebuffer);
bool backend_client_stop_stream(const char *reason);
bool backend_client_send_metrics(const char *json);
/* Speaker task queues completion; main task performs the WebSocket send. */
void backend_client_queue_playback_stop(uint32_t audio_id, uint32_t generation, bool success);
void backend_client_service_playback(void);
