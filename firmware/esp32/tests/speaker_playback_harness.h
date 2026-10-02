#pragma once
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_conversion.h"
#include "playback_buffer.h"
#include "stream_policy.h"
#include "esp_timer.h"
#include "freertos/queue.h"

/* Exit directly on failure: MinGW's CRT assert may open a Windows crash dialog. */
#undef assert
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_LOG_WARN 1

static bool speaker_tx_enabled;
static atomic_bool playback_active;
static void *i2s_tx_handle;

static void vTaskDelay(TickType_t ticks);
static esp_err_t i2s_channel_enable(void *handle);
static esp_err_t i2s_channel_disable(void *handle);
static esp_err_t i2s_channel_preload_data(void *handle, const void *src,
                                       size_t size, size_t *loaded);
static esp_err_t i2s_channel_write(void *handle, const void *src, size_t size,
                                 size_t *written, uint32_t timeout_ms);
static void backend_client_queue_playback_stop(uint32_t id, uint32_t generation,
                                              bool success);
static void device_diagnostics_event(int level, const char *format, ...);
