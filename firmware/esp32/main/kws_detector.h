#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool triggered;
    float score;
    uint32_t inference_cycles;
    uint32_t inference_us;
} kws_detection_t;

typedef struct {
    uint32_t inference_count;
    uint32_t keyword_hits;
    uint32_t false_activation_count;
    uint32_t last_inference_cycles;
    uint32_t average_inference_cycles;
    uint32_t last_inference_us;
    uint32_t average_inference_us;
    float last_score;
    float peak_score;
    float peak_rms_100ms_dbfs;
    uint32_t inference_interval_ms;
    bool selftest_passed;
    size_t model_flash_bytes;
    size_t tensor_arena_bytes;
    size_t tensor_arena_used_bytes;
    size_t free_heap_before_bytes;
    size_t free_heap_after_bytes;
    size_t minimum_free_heap_bytes;
} kws_stats_t;

#ifdef __cplusplus
extern "C" {
#endif

bool kws_detector_init(void);
bool kws_detector_process(const int16_t *samples, size_t sample_count,
                          kws_detection_t *detection);
// Snapshot and reset interval peaks; main's 5-second reporter is sole consumer.
void kws_detector_get_stats(kws_stats_t *stats);
void kws_detector_reset_window(void);
void kws_detector_mark_false_activation(void);

#ifdef __cplusplus
}
#endif
