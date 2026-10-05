// Hello/Tors keyword spotting on ESP32-S3.
//
// mic (16 kHz) -> conditioning -> streaming MFCC (one 49x40 window per 20 ms hop)
// -> int8 DS-CNN (TFLite Micro) -> threshold + smoothing -> log / LED.
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "hello_tors_model_data.h"
#include "kws_audio.h"
#include "kws_config.h"
#include "kws_detector.h"
#include "kws_features.h"
#include "kws_mic.h"
#if KWS_RUN_SELF_TEST
#include "kws_test_vectors.h"
#endif

// A retrain with different features must fail the build, not silently degrade accuracy.
static_assert(KWS_MODEL_NUM_FRAMES == KWS_NUM_FRAMES, "model/feature frame count mismatch");
static_assert(KWS_MODEL_NUM_MFCC == KWS_NUM_MFCC, "model/feature MFCC count mismatch");
static_assert(KWS_MODEL_SAMPLE_RATE == KWS_SAMPLE_RATE, "model/feature sample rate mismatch");
static_assert(KWS_MODEL_FRAME_LEN == KWS_FRAME_LEN, "model/feature frame length mismatch");
static_assert(KWS_MODEL_FRAME_STEP == KWS_FRAME_STEP, "model/feature frame step mismatch");

namespace {

const char *TAG = "kws";
constexpr int kFeatures = KWS_NUM_FRAMES * KWS_NUM_MFCC;
constexpr int64_t kHopUs = 1000000LL * KWS_FRAME_STEP / KWS_SAMPLE_RATE;  // 20 ms

alignas(16) uint8_t g_arena[KWS_TENSOR_ARENA_SIZE];
tflite::MicroInterpreter *g_interp = nullptr;
TfLiteTensor *g_in = nullptr;
TfLiteTensor *g_out = nullptr;

bool setup_model() {
    const tflite::Model *model = tflite::GetModel(g_hello_tors_model_data);
#ifdef TFLITE_SCHEMA_VERSION
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema %d != supported %d", (int)model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }
#endif
    // Exactly the ops in hello_tors_kws_int8.tflite. The first four only resolve the
    // model's dynamic batch dimension (Keras export); TFLM uses the static [1,49,40] shape.
    static tflite::MicroMutableOpResolver<9> resolver;
    resolver.AddShape();
    resolver.AddStridedSlice();
    resolver.AddPack();
    resolver.AddReshape();
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddMean();
    resolver.AddFullyConnected();
    resolver.AddSoftmax();

    static tflite::MicroInterpreter interp(model, resolver, g_arena, sizeof(g_arena));
    if (interp.AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed - increase KWS_TENSOR_ARENA_SIZE");
        return false;
    }
    g_interp = &interp;
    g_in = interp.input(0);
    g_out = interp.output(0);
    if (g_in->type != kTfLiteInt8 || g_in->bytes != (size_t)kFeatures || g_out->type != kTfLiteInt8) {
        ESP_LOGE(TAG, "unexpected model I/O (input type %d, %u bytes)", g_in->type, (unsigned)g_in->bytes);
        return false;
    }
    ESP_LOGI(TAG, "model: %u bytes, arena used %u / %u, input scale %.6f zp %d",
             g_hello_tors_model_data_len, (unsigned)interp.arena_used_bytes(), (unsigned)sizeof(g_arena),
             g_in->params.scale, (int)g_in->params.zero_point);
    return true;
}

// Runs the model on whatever is in the input tensor; returns P(keyword).
float run_model(int8_t *raw_out = nullptr) {
    if (g_interp->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke failed");
        return 0.0f;
    }
    if (raw_out) {
        raw_out[0] = g_out->data.int8[0];
        raw_out[1] = g_out->data.int8[1];
    }
    return (g_out->data.int8[KWS_KEYWORD_INDEX] - g_out->params.zero_point) * g_out->params.scale;
}

#if KWS_RUN_SELF_TEST
// Same check as tools/pc_harness: features must match the TensorFlow reference and the
// model output must match the PC TFLite interpreter.
bool self_test_one(const char *name, const int16_t *pcm, const int8_t *exp_feat, const int8_t *exp_out) {
    float frame[KWS_FRAME_LEN], mfcc[KWS_NUM_MFCC];
    int exact = 0, off1 = 0, worse = 0;
    int64_t t0 = esp_timer_get_time();
    for (int f = 0; f < KWS_NUM_FRAMES; f++) {
        for (int i = 0; i < KWS_FRAME_LEN; i++) frame[i] = pcm[f * KWS_FRAME_STEP + i] / 32768.0f;
        kws_mfcc_frame(frame, mfcc);
        for (int k = 0; k < KWS_NUM_MFCC; k++) {
            int idx = f * KWS_NUM_MFCC + k;
            int8_t q = kws_quantize(mfcc[k], g_in->params.scale, g_in->params.zero_point);
            g_in->data.int8[idx] = q;
            int d = std::abs(q - exp_feat[idx]);
            if (d == 0) exact++; else if (d == 1) off1++; else worse++;
        }
    }
    int64_t t1 = esp_timer_get_time();
    int8_t raw[2];
    float p = run_model(raw);
    int64_t t2 = esp_timer_get_time();
    bool ok = worse == 0 && off1 <= kFeatures / 100 &&
              std::abs(raw[KWS_KEYWORD_INDEX] - exp_out[KWS_KEYWORD_INDEX]) <= 2;
    ESP_LOGI(TAG, "self-test %-9s features %d exact / %d off-by-1 / %d worse | P(kw) %.3f raw %d (PC %d) | "
                  "49 frames %lld us, invoke %lld us -> %s",
             name, exact, off1, worse, p, raw[KWS_KEYWORD_INDEX], exp_out[KWS_KEYWORD_INDEX],
             (long long)(t1 - t0), (long long)(t2 - t1), ok ? "ok" : "MISMATCH");
    return ok;
}
#endif

// Pick an inference stride the CPU can sustain in real time.
int choose_stride() {
    for (int i = 0; i < kFeatures; i++) g_in->data.int8[i] = 0;
    run_model();  // first call can be slower (caches)
    int64_t t0 = esp_timer_get_time();
    constexpr int kRuns = 5;
    for (int i = 0; i < kRuns; i++) run_model();
    int64_t invoke_us = (esp_timer_get_time() - t0) / kRuns;

    float frame[KWS_FRAME_LEN] = {0}, mfcc[KWS_NUM_MFCC];
    t0 = esp_timer_get_time();
    kws_mfcc_frame(frame, mfcc);
    int64_t frame_us = esp_timer_get_time() - t0;

    // Per hop we always compute one MFCC frame; inference gets the rest, with 25% margin.
    int64_t budget = kHopUs - frame_us;
    if (budget < 1000) budget = 1000;
    int need = (int)((invoke_us * 5 / 4 + budget - 1) / budget);
    int stride = need > KWS_INFERENCE_EVERY_HOPS ? need : KWS_INFERENCE_EVERY_HOPS;
    ESP_LOGI(TAG, "timing: MFCC frame %lld us, invoke %lld us, hop %lld us -> inference every %d hop(s)",
             (long long)frame_us, (long long)invoke_us, (long long)kHopUs, stride);
    if (stride > KWS_INFERENCE_EVERY_HOPS)
        ESP_LOGW(TAG, "inference too slow for every hop; SMOOTHING_WINDOW %d now spans %d ms",
                 SMOOTHING_WINDOW, (int)(SMOOTHING_WINDOW * stride * kHopUs / 1000));
    return stride;
}

}  // namespace

extern "C" void app_main(void) {
    kws_features_init();
    if (!setup_model()) return;

#if KWS_RUN_SELF_TEST
    bool ok = self_test_one("keyword", kws_test_pos_pcm, kws_test_pos_features, kws_test_pos_output);
    ok &= self_test_one("near-miss", kws_test_neg_pcm, kws_test_neg_features, kws_test_neg_output);
    if (ok) ESP_LOGI(TAG, "SELF-TEST PASS - device matches the PC reference");
    else ESP_LOGE(TAG, "SELF-TEST FAIL - device output differs from the PC reference; detections will not match");
#endif

    const int stride = choose_stride();
    ESP_ERROR_CHECK(kws_mic_init());

    static kws_conditioner_t cond;
    kws_conditioner_init(&cond, KWS_INPUT_GAIN_DB, KWS_NOISE_FLOOR_DB, KWS_DC_BLOCK);
    static kws_stream_t stream;
    kws_stream_reset(&stream);
    static kws_detector_t det;
    kws_detector_init(&det, DETECTION_THRESHOLD, SMOOTHING_WINDOW,
                      (int)(REFRACTORY_MS * 1000LL / kHopUs));

    if (KWS_LED_GPIO >= 0) {
        gpio_reset_pin((gpio_num_t)KWS_LED_GPIO);
        gpio_set_direction((gpio_num_t)KWS_LED_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)KWS_LED_GPIO, !KWS_LED_ACTIVE_LEVEL);
    }

    ESP_LOGI(TAG, "listening: threshold %.2f, smooth %d, gain %.1f dB, noise floor %s - say \"Hello Tors\"",
             DETECTION_THRESHOLD, SMOOTHING_WINDOW, KWS_INPUT_GAIN_DB,
             KWS_NOISE_FLOOR_DB > -120.0f ? "on" : "off");

    float hop[KWS_FRAME_STEP];
    uint32_t hop_count = 0;
    int hops_since_infer = 0;
    int64_t led_off_at = 0;
    // Level stats over 100 ms blocks (same measure as the training-level numbers).
    double block_energy = 0.0;
    int block_samples = 0, stat_hops = 0;
    float level_peak_db = -200.0f, level_min_db = 0.0f, p_max = 0.0f;

    while (true) {
        if (kws_mic_read(hop, KWS_FRAME_STEP) != ESP_OK) {
            ESP_LOGE(TAG, "mic read failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        for (int i = 0; i < KWS_FRAME_STEP; i++) block_energy += (double)hop[i] * hop[i];
        block_samples += KWS_FRAME_STEP;
        if (block_samples >= KWS_SAMPLE_RATE / 10) {
            float db = 10.0f * log10f((float)(block_energy / block_samples) + 1e-12f);
            level_peak_db = fmaxf(level_peak_db, db);
            level_min_db = fminf(level_min_db, db);
            block_energy = 0.0;
            block_samples = 0;
        }

        kws_conditioner_process(&cond, hop, KWS_FRAME_STEP);
        kws_stream_push(&stream, hop);
        hop_count++;

        if (++hops_since_infer >= stride) {
            kws_stream_quantize(&stream, g_in->data.int8, g_in->params.scale, g_in->params.zero_point);
            float p = run_model();
            if (hop_count > KWS_WARMUP_HOPS) {
                p_max = fmaxf(p_max, p);
                if (kws_detector_update(&det, p, hops_since_infer)) {
                    ESP_LOGI(TAG, ">>> HELLO TORS  P=%.3f  t=%.2f s", p,
                             hop_count * (double)kHopUs / 1e6);
                    if (KWS_LED_GPIO >= 0) {
                        gpio_set_level((gpio_num_t)KWS_LED_GPIO, KWS_LED_ACTIVE_LEVEL);
                        led_off_at = esp_timer_get_time() + KWS_LED_ON_MS * 1000LL;
                    }
                }
            }
            hops_since_infer = 0;
        }

        if (led_off_at && esp_timer_get_time() >= led_off_at) {
            gpio_set_level((gpio_num_t)KWS_LED_GPIO, !KWS_LED_ACTIVE_LEVEL);
            led_off_at = 0;
        }

#if KWS_LOG_LEVELS
        if (++stat_hops >= KWS_SAMPLE_RATE / KWS_FRAME_STEP) {  // once per second
            ESP_LOGI(TAG, "mic: peak %.1f dBFS (after gain %.1f), quietest %.1f dBFS | max P(kw) %.2f",
                     level_peak_db, level_peak_db + KWS_INPUT_GAIN_DB, level_min_db, p_max);
            stat_hops = 0;
            level_peak_db = -200.0f;
            level_min_db = 0.0f;
            p_max = 0.0f;
        }
#else
        (void)stat_hops;
#endif
    }
}
