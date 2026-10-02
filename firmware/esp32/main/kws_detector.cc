#include "kws_detector.h"
#include "kws_frontend.h"
#include "stream_policy.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "kws_model_data.h"
#include "kws_test_vectors.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Previous measured allocation: 108508 B. Fail closed if allocation/self-test fails.
#define KWS_TENSOR_ARENA_BYTES (112 * 1024)
static const char *TAG = "hello_tors_kws";
alignas(16) static uint8_t tensor_arena[KWS_TENSOR_ARENA_BYTES];
static kws_frontend_t frontend;
static kws_decision_t decision;
static tflite::MicroMutableOpResolver<9> resolver;
static tflite::MicroInterpreter *interpreter;
static TfLiteTensor *input_tensor, *output_tensor;
static unsigned inference_stride = 1, hops_since_inference;
static bool window_inferred, initialized;
static uint32_t feature_budget_us, invoke_budget_us;
static uint64_t level_energy;
static unsigned level_hops;
static kws_stats_t stats;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;

static bool valid_tensor_contract(void) {
    if (!input_tensor || !output_tensor || input_tensor->type != kTfLiteInt8 ||
        output_tensor->type != kTfLiteInt8 || input_tensor->bytes != 1960 ||
        output_tensor->bytes != 2 || !input_tensor->dims || !output_tensor->dims)
        return false;
    const auto *in = input_tensor->dims;
    const auto *out = output_tensor->dims;
    return in->size == 3 && in->data[0] == 1 && in->data[1] == 49 && in->data[2] == 40 &&
           out->size == 2 && out->data[0] == 1 && out->data[1] == 2 &&
           fabsf(input_tensor->params.scale - KWS_INPUT_SCALE) < 1e-7f &&
           input_tensor->params.zero_point == KWS_INPUT_ZERO_POINT &&
           fabsf(output_tensor->params.scale - 1.0f / 256.0f) < 1e-7f &&
           output_tensor->params.zero_point == -128;
}

static bool selftest(const char *name, const int16_t *pcm,
                     const int8_t *expected_features, const int8_t *expected_output) {
    unsigned off1 = 0, worse = 0;
    float mfcc[KWS_NUM_MFCC];
    // Reuse streaming history as boot scratch; vectors/tables live in flash.
    for (int f = 0; f < KWS_NUM_FRAMES; ++f) {
        const int64_t start = esp_timer_get_time();
        for (int i = 0; i < KWS_FRAME_LEN; ++i)
            frontend.history[i] = pcm[f * KWS_FRAME_STEP + i] / 32768.0f;
        kws_mfcc_frame(frontend.history, mfcc);
        for (int m = 0; m < KWS_NUM_MFCC; ++m) {
            int i = f * KWS_NUM_MFCC + m;
            int8_t q = kws_quantize(mfcc[m], KWS_INPUT_SCALE, KWS_INPUT_ZERO_POINT);
            input_tensor->data.int8[i] = q;
            int delta = abs((int)q - expected_features[i]);
            if (delta == 1) ++off1;
            else if (delta > 1) ++worse;
        }
        const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
        if (elapsed > feature_budget_us) feature_budget_us = elapsed;
    }
    const int64_t start = esp_timer_get_time();
    const TfLiteStatus result = interpreter->Invoke();
    const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
    if (elapsed > invoke_budget_us) invoke_budget_us = elapsed;
    const bool output_ok = result == kTfLiteOk &&
        abs((int)output_tensor->data.int8[0] - expected_output[0]) <= 2 &&
        abs((int)output_tensor->data.int8[1] - expected_output[1]) <= 2;
    const bool ok = worse == 0 && off1 <= 19 && output_ok;
    ESP_LOGI(TAG, "SELFTEST %s: %s features_exact=%u/1960 off1=%u worse=%u output=[%d,%d] expected=[%d,%d] invoke=%uus",
             name, ok ? "PASS" : "FAIL", 1960 - off1 - worse, off1, worse,
             output_tensor->data.int8[0], output_tensor->data.int8[1],
             expected_output[0], expected_output[1], (unsigned)elapsed);
    return ok;
}

bool kws_detector_init(void) {
    if (initialized) return true;
    if (resolver.AddShape() != kTfLiteOk || resolver.AddStridedSlice() != kTfLiteOk ||
        resolver.AddPack() != kTfLiteOk || resolver.AddReshape() != kTfLiteOk ||
        resolver.AddConv2D() != kTfLiteOk || resolver.AddDepthwiseConv2D() != kTfLiteOk ||
        resolver.AddMean() != kTfLiteOk || resolver.AddFullyConnected() != kTfLiteOk ||
        resolver.AddSoftmax() != kTfLiteOk) {
        ESP_LOGE(TAG, "Failed to register model operators");
        return false;
    }
    const auto *model = tflite::GetModel(g_hello_tors_model_data);
    if (!model || model->version() != TFLITE_SCHEMA_VERSION) return false;
    static tflite::MicroInterpreter runtime(model, resolver, tensor_arena, sizeof(tensor_arena));
    interpreter = &runtime;
    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "TFLite allocation failed; arena=%u B", (unsigned)sizeof(tensor_arena));
        return false;
    }
    input_tensor = interpreter->input(0);
    output_tensor = interpreter->output(0);
    if (!valid_tensor_contract()) {
        ESP_LOGE(TAG, "Model input/output shape, type or quantization differs from supplied package");
        return false;
    }
    kws_features_init();
    const bool pos_ok = selftest("positive", kws_test_pos_pcm, kws_test_pos_features, kws_test_pos_output);
    const bool neg_ok = selftest("negative", kws_test_neg_pcm, kws_test_neg_features, kws_test_neg_output);
    if (!pos_ok || !neg_ok) return false;
    inference_stride = kws_inference_stride(invoke_budget_us, feature_budget_us,
                                            portTICK_PERIOD_MS * 1000);
    if (!inference_stride) {
        ESP_LOGE(TAG, "Feature processing exceeds 20 ms capture budget: %uus", (unsigned)feature_budget_us);
        return false;
    }
    memset(&stats, 0, sizeof(stats));
    stats.model_flash_bytes = sizeof(g_hello_tors_model_data);
    stats.tensor_arena_bytes = sizeof(tensor_arena);
    stats.tensor_arena_used_bytes = interpreter->arena_used_bytes();
    stats.minimum_free_heap_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    stats.inference_interval_ms = inference_stride * 20;
    stats.peak_rms_100ms_dbfs = -120.0f;
    stats.selftest_passed = true;
    kws_detector_reset_window();
    initialized = true;
    ESP_LOGI(TAG, "KWS ready: threshold=%.2f consecutive=%d refractory=1000ms stride=%u hops/%ums feature_max=%uus",
             KWS_DETECTION_THRESHOLD, KWS_REQUIRED_POSITIVES, inference_stride,
             inference_stride * 20, (unsigned)feature_budget_us);
    ESP_LOGI(TAG, "Model=%u B arena=%u/%u B; PCM/32768 -> package MFCC -> int8; no optional conditioning",
             (unsigned)stats.model_flash_bytes, (unsigned)stats.tensor_arena_used_bytes,
             (unsigned)stats.tensor_arena_bytes);
    return true;
}

bool kws_detector_process(const int16_t *samples, size_t sample_count, kws_detection_t *detection) {
    if (detection) memset(detection, 0, sizeof(*detection));
    if (!initialized || !samples || !detection || sample_count != KWS_FRAME_STEP) return false;
    for (size_t i = 0; i < sample_count; ++i) {
        int64_t v = samples[i];
        level_energy += (uint64_t)(v * v);
    }
    if (++level_hops == 5) {
        const float rms = sqrtf((float)level_energy / (5 * KWS_FRAME_STEP));
        const float dbfs = rms > 0 ? 20.0f * log10f(rms / 32768.0f) : -120.0f;
        portENTER_CRITICAL(&stats_lock);
        if (dbfs > stats.peak_rms_100ms_dbfs) stats.peak_rms_100ms_dbfs = dbfs;
        portEXIT_CRITICAL(&stats_lock);
        level_hops = 0;
        level_energy = 0;
    }
    const bool ready = kws_frontend_push(&frontend, samples);
    if (!ready) return true;
    if (window_inferred && ++hops_since_inference < inference_stride) return true;
    hops_since_inference = 0;
    window_inferred = true;
    memcpy(input_tensor->data.int8, frontend.features, sizeof(frontend.features));
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t cycle_start = esp_cpu_get_cycle_count();
    const int64_t start = esp_timer_get_time();
    const TfLiteStatus result = interpreter->Invoke();
    const uint32_t cycles = esp_cpu_get_cycle_count() - cycle_start;
    const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
    const size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (result != kTfLiteOk) {
        decision.consecutive = 0;
        ESP_LOGE(TAG, "TFLite invoke failed");
        return false;
    }
    // Never decrease cadence margin during this boot. Timing includes preemption.
    if (elapsed > invoke_budget_us) {
        invoke_budget_us = elapsed;
        const unsigned next = kws_inference_stride(invoke_budget_us, feature_budget_us,
                                                    portTICK_PERIOD_MS * 1000);
        if (next > inference_stride) inference_stride = next;
    }
    const float score = kws_probability(output_tensor->data.int8[1],
        output_tensor->params.scale, output_tensor->params.zero_point);
    detection->score = score;
    detection->inference_cycles = cycles;
    detection->inference_us = elapsed;
    detection->triggered = kws_decision_update(&decision, score, esp_timer_get_time());
    portENTER_CRITICAL(&stats_lock);
    ++stats.inference_count;
    stats.last_inference_cycles = cycles;
    stats.last_inference_us = elapsed;
    stats.average_inference_cycles = (uint32_t)(((uint64_t)stats.average_inference_cycles *
        (stats.inference_count - 1) + cycles) / stats.inference_count);
    stats.average_inference_us = (uint32_t)(((uint64_t)stats.average_inference_us *
        (stats.inference_count - 1) + elapsed) / stats.inference_count);
    stats.last_score = score;
    if (score > stats.peak_score) stats.peak_score = score;
    stats.free_heap_before_bytes = before;
    stats.free_heap_after_bytes = after;
    if (after < stats.minimum_free_heap_bytes) stats.minimum_free_heap_bytes = after;
    stats.inference_interval_ms = inference_stride * 20;
    if (detection->triggered) ++stats.keyword_hits;
    portEXIT_CRITICAL(&stats_lock);
    return true;
}

void kws_detector_get_stats(kws_stats_t *destination) {
    if (!destination) return;
    // Main task's 5-second reporting is the sole consumer of interval peaks.
    portENTER_CRITICAL(&stats_lock);
    *destination = stats;
    stats.peak_score = 0.0f;
    stats.peak_rms_100ms_dbfs = -120.0f;
    portEXIT_CRITICAL(&stats_lock);
}

void kws_detector_mark_false_activation(void) {
    portENTER_CRITICAL(&stats_lock);
    ++stats.false_activation_count;
    portEXIT_CRITICAL(&stats_lock);
}

void kws_detector_reset_window(void) {
    // Boot then inference-task-only. Keep refractory time across capture gaps.
    kws_frontend_reset(&frontend);
    hops_since_inference = 0;
    window_inferred = false;
    decision.consecutive = 0;
    level_energy = 0;
    level_hops = 0;
}
