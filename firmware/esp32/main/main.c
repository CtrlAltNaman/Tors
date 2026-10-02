#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "audio_conversion.h"
#include "backend_client.h"
#include "kws_detector.h"
#include "network_config.h"
#include "stream_policy.h"
#include "diagnostic_store.h"
#include "device_diagnostics.h"
#include "device_leds.h"
#include "local_recordings.h"

#define I2S_BCLK_PIN GPIO_NUM_4
#define I2S_WS_PIN GPIO_NUM_5
#define I2S_DATA_PIN GPIO_NUM_6
#define I2S_AMP_DATA_PIN GPIO_NUM_8

#define BUTTON_PIN GPIO_NUM_16

#define SAMPLE_RATE 16000
#define AUDIO_BUFFER_SAMPLES AUDIO_FRAME_SAMPLES
#define BUTTON_DEBOUNCE_MS 200
#define USB_RX_BUFFER_SIZE 256
#define USB_TX_BUFFER_SIZE 4096
#define AUDIO_TX_BUFFER_SAMPLES 256
#define I2S_DMA_DESCRIPTORS 8
/* Full TX ring plus one frame of scheduling margin, at 16 kHz. */
#define SPEAKER_DRAIN_MS ((I2S_DMA_DESCRIPTORS + 1) * AUDIO_FRAME_MS)
#define SPEAKER_IDLE_WAIT_MS 250
#define KWS_QUEUE_FRAMES 16
#define STREAM_PREBUFFER_FRAMES 40 /* Leave 400 ms of ring headroom for connection sends. */
#define RED_PULSE_US 300000
#define METRICS_PERIOD_MS 5000

static const char *TAG = "edgeai_kws";

static i2s_chan_handle_t i2s_rx_handle;
static i2s_chan_handle_t i2s_tx_handle;
static int32_t i2s_buffer[AUDIO_BUFFER_SAMPLES];
static int16_t pcm_buffer[AUDIO_BUFFER_SAMPLES];
static int32_t audio_tx_buffer[AUDIO_TX_BUFFER_SAMPLES];
/* Task-context only; never accessed by the cache-disabled I2S ISR. */
static EXT_RAM_BSS_ATTR audio_history_t audio_history;
static uint32_t history_epoch[AUDIO_HISTORY_FRAMES];
static portMUX_TYPE audio_lock = portMUX_INITIALIZER_UNLOCKED;
static ambient_tracker_t ambient;
static voice_endpoint_t endpoint;
static QueueHandle_t kws_queue;
static TaskHandle_t capture_handle, kws_handle, speaker_handle;
static atomic_bool playback_active;
typedef struct {
    uint64_t index;
    int16_t samples[AUDIO_FRAME_SAMPLES];
} kws_frame_t;
typedef struct {
    bool active, ending, network_enabled, starting;
    uint32_t id;
    uint64_t first_frame, live_frame, end_frame;
    int64_t detected_ms;
    uint32_t capture_epoch;
} stream_session_t;
static stream_session_t session;
static uint64_t listen_from_frame;
static int64_t red_until_us;
static uint32_t dma_overflows, capture_errors, kws_queue_drops, stream_overflows;
static uint32_t dropped_triggers;
/* Main-task-owned counters: increment only after a complete frame is sent. */
static uint64_t websocket_frames_sent;
static uint64_t metrics_previous_frames, metrics_previous_sent;
static int64_t metrics_previous_time_us;
static float last_rms;

typedef struct {
    uint16_t frequency_hz;
    uint16_t duration_ms;
} melody_note_t;

/* Two short, smooth startup beeps with a brief pause between them. */
static const melody_note_t boot_beeps[] = {
    {880, 180},
    {0, 120},
    {880, 180},
};

#define MELODY_AMPLITUDE 7000.0f
#define TWO_PI 6.28318530717958647692f

static atomic_bool recording;
static int last_button_state = 1;
static uint64_t usb_frame_cursor;
static uint32_t next_stream_id = 1;
static int64_t next_metrics_time_us;
static bool speaker_tx_enabled = true;

static void fatal_error(const char *operation, esp_err_t error) {
    ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(error));
    device_leds_set(DEVICE_ERROR);
    while (true) vTaskDelay(pdMS_TO_TICKS(200));
}

static esp_err_t init_gpio(void) {
    esp_err_t error = device_leds_init();
    if (error != ESP_OK) {
        return error;
    }

    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    error = gpio_config(&button_config);
    if (error != ESP_OK) {
        return error;
    }

    return ESP_OK;
}

static recording_frame_result_t copy_recording_frame(uint32_t id, uint64_t index, int16_t samples[320]) {
    portENTER_CRITICAL(&audio_lock);
    recording_frame_result_t result;
    if (id != session.id) result = REC_FRAME_LOST;
    else if (session.ending && index >= session.end_frame) result = REC_FRAME_END;
    else if (index >= audio_history.total_frames) result = REC_FRAME_WAIT;
    else if (!audio_history_copy(&audio_history, index, samples) ||
             history_epoch[index % AUDIO_HISTORY_FRAMES] != session.capture_epoch)
        result = REC_FRAME_LOST;
    else result = REC_FRAME_DATA;
    portEXIT_CRITICAL(&audio_lock);
    return result;
}

static esp_err_t init_recorder_transport(void) {
    if (usb_serial_jtag_is_driver_installed()) {
        return ESP_OK;
    }

    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = USB_TX_BUFFER_SIZE,
        .rx_buffer_size = USB_RX_BUFFER_SIZE,
    };

    return usb_serial_jtag_driver_install(&config);
}

static bool send_recorder_bytes(const void *data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        const int written = usb_serial_jtag_write_bytes(
            (const uint8_t *)data + offset,
            length - offset,
            pdMS_TO_TICKS(20));
        if (written <= 0) {
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

static void send_recorder_text(const char *text) {
    const size_t length = strlen(text);
    if (!send_recorder_bytes(text, length)) {
        ESP_LOGE(TAG, "USB recorder text write failed");
    }
}

static bool IRAM_ATTR on_capture_overflow(i2s_chan_handle_t handle,
                                         i2s_event_data_t *event, void *context) {
    (void)handle; (void)event; (void)context;
    portENTER_CRITICAL_ISR(&audio_lock);
    dma_overflows++;
    portEXIT_CRITICAL_ISR(&audio_lock);
    return false;
}

static esp_err_t init_i2s(void) {
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_frame_num = AUDIO_FRAME_SAMPLES;
    channel_config.dma_desc_num = I2S_DMA_DESCRIPTORS; /* 160 ms; reader runs every 20 ms. */
    channel_config.auto_clear_after_cb = true;

    esp_err_t error = i2s_new_channel(&channel_config, &i2s_tx_handle, &i2s_rx_handle);
    if (error != ESP_OK) {
        return error;
    }

    i2s_std_config_t rx_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_DATA_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    rx_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    error = i2s_channel_init_std_mode(i2s_rx_handle, &rx_config);
    if (error != ESP_OK) {
        return error;
    }

    i2s_std_config_t tx_config = rx_config;
    tx_config.gpio_cfg.dout = I2S_AMP_DATA_PIN;
    tx_config.gpio_cfg.din = I2S_GPIO_UNUSED;

    error = i2s_channel_init_std_mode(i2s_tx_handle, &tx_config);
    if (error != ESP_OK) {
        return error;
    }

    const i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = on_capture_overflow};
    error = i2s_channel_register_event_callback(i2s_rx_handle, &callbacks, NULL);
    if (error != ESP_OK) return error;

    error = i2s_channel_enable(i2s_rx_handle);
    if (error != ESP_OK) {
        return error;
    }

    return i2s_channel_enable(i2s_tx_handle);
}

static void capture_task(void *argument) {
    (void)argument;
    size_t filled = 0;
    const int64_t settling_until = esp_timer_get_time() + 300000;
    bool settled = false;
    kws_frame_t frame;
    for (;;) {
        size_t received = 0;
        const esp_err_t error = i2s_channel_read(i2s_rx_handle,
            (uint8_t *)i2s_buffer + filled, sizeof(i2s_buffer) - filled, &received, 100);
        if (error != ESP_OK && error != ESP_ERR_TIMEOUT) {
            portENTER_CRITICAL(&audio_lock);
            capture_errors++;
            portEXIT_CRITICAL(&audio_lock);
            filled = 0;
            vTaskDelay(1);
            continue;
        }
        filled += received;
        if (filled < sizeof(i2s_buffer)) continue;
        filled = 0;
        if (esp_timer_get_time() < settling_until) continue;
        if (!settled) {
            portENTER_CRITICAL(&audio_lock);
            dma_overflows = 0; /* Ignore intentional boot-beep warmup discard. */
            portEXIT_CRITICAL(&audio_lock);
            settled = true;
        }
        convert_i2s_to_pcm(i2s_buffer, pcm_buffer, AUDIO_FRAME_SAMPLES);
        const float rms = audio_ac_rms(pcm_buffer);
        const bool playing = atomic_load(&playback_active);
        local_recordings_stats_t local;
        local_recordings_get_stats(&local);
        const int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&audio_lock);
        frame.index = audio_history.total_frames;
        audio_history_push(&audio_history, pcm_buffer);
        history_epoch[frame.index % AUDIO_HISTORY_FRAMES] = dma_overflows + capture_errors;
        last_rms = rms;
        const bool was_ready = ambient.ready;
        if (!session.active && !playing) ambient_observe(&ambient, rms);
        if (session.active && !session.ending && voice_endpoint_observe(&endpoint, rms)) {
            session.ending = true;
            session.end_frame = audio_history.total_frames;
        }
        const bool active = session.active;
        const bool calibrated = ambient.ready;
        const bool run_kws = calibrated && !active && !playing && !local.active && !local.preparing;
        const bool triggered = now < red_until_us;
        const bool calibrated_now = !was_ready && ambient.ready;
        const float noise = ambient.noise_rms;
        portEXIT_CRITICAL(&audio_lock);
        local_recordings_allow_maintenance(!active && !playing && !atomic_load(&recording));
        device_leds_set(device_state_resolve((device_inputs_t){
            .trigger=triggered, .recording=active || local.active || atomic_load(&recording),
            .playback=playing, .preparing=local.preparing, .calibrated=calibrated,
            .backend_ready=backend_client_is_ready(),
        }));
        if (calibrated_now) ESP_LOGI(TAG, "Listening: ambient_rms=%.1f speech_threshold=%.1f",
                                     noise, fmaxf(VOICE_RMS_MIN, noise * VOICE_NOISE_RATIO));
        if (run_kws) {
            memcpy(frame.samples, pcm_buffer, sizeof(frame.samples));
            if (xQueueSend(kws_queue, &frame, 0) != pdTRUE) {
                portENTER_CRITICAL(&audio_lock);
                kws_queue_drops++;
                portEXIT_CRITICAL(&audio_lock);
            }
        }
    }
}

static void inference_task(void *argument) {
    (void)argument;
    kws_frame_t frame;
    uint64_t expected_frame = UINT64_MAX;
    for (;;) {
        if (xQueueReceive(kws_queue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
            kws_detector_reset_window();
            expected_frame = UINT64_MAX;
            continue;
        }
        portENTER_CRITICAL(&audio_lock);
        const bool skip = session.active || frame.index < listen_from_frame;
        portEXIT_CRITICAL(&audio_lock);
        if (skip || atomic_load(&playback_active) || local_recordings_busy()) {
            kws_detector_reset_window();
            expected_frame = UINT64_MAX;
            continue;
        }
        if (frame.index != expected_frame) kws_detector_reset_window();
        expected_frame = frame.index + 1;
        kws_detection_t detection = {0};
        if (kws_detector_process(frame.samples, AUDIO_FRAME_SAMPLES, &detection) && detection.triggered) {
            const bool ready = backend_client_is_ready();
            const int64_t now = esp_timer_get_time();
            portENTER_CRITICAL(&audio_lock);
            red_until_us = now + RED_PULSE_US;
            bool new_session = !session.active;
            if (new_session) {
                session = (stream_session_t){
                    .active = true, .starting = true, .network_enabled = ready, .id = next_stream_id++, .detected_ms = now / 1000,
                    .live_frame = audio_history.total_frames,
                    .first_frame = audio_history.total_frames > STREAM_PREBUFFER_FRAMES
                        ? audio_history.total_frames - STREAM_PREBUFFER_FRAMES : 0,
                };
                session.capture_epoch = history_epoch[session.first_frame % AUDIO_HISTORY_FRAMES];
                voice_endpoint_begin(&endpoint, ambient.noise_rms);
            }
            const uint32_t id = session.id;
            const uint64_t first = session.first_frame;
            if (!ready) dropped_triggers++;
            portEXIT_CRITICAL(&audio_lock);
            bool saved = new_session && local_recordings_begin(id, first);
            if (new_session) {
                portENTER_CRITICAL(&audio_lock);
                session.starting = false;
                if (!saved && !ready) session.active = false;
                portEXIT_CRITICAL(&audio_lock);
            }
            device_diagnostics_event(ready ? ESP_LOG_INFO : ESP_LOG_WARN,
                "Hello Tors score=%.3f; red pulse 300ms; upload=%s local=%s", detection.score,
                ready ? "requested" : "offline", saved ? "recording" : "unavailable");
            kws_detector_reset_window();
            expected_frame = UINT64_MAX;
        }
        if (detection.inference_us != 0) vTaskDelay(1);
    }
}

static void finish_session(const char *reason) {
    backend_client_stop_stream(reason);
    portENTER_CRITICAL(&audio_lock);
    session.active = false;
    if (!session.ending) {
        session.ending = true;
        session.end_frame = audio_history.total_frames;
    }
    listen_from_frame = audio_history.total_frames;
    portEXIT_CRITICAL(&audio_lock);
    device_diagnostics_event(ESP_LOG_INFO, "Session stopped: %s; listening resumes after local save/preparation", reason);
}

static void disable_upload(const char *reason) {
    backend_client_stop_stream(reason);
    portENTER_CRITICAL(&audio_lock);
    session.network_enabled = false;
    portEXIT_CRITICAL(&audio_lock);
    device_diagnostics_event(ESP_LOG_WARN, "Upload stopped: %s; local recorder continues independently", reason);
}

static void service_stream(void) {
    static uint32_t sending_id;
    static uint64_t cursor;
    int16_t samples[AUDIO_FRAME_SAMPLES];
    portENTER_CRITICAL(&audio_lock);
    const stream_session_t current = session;
    portEXIT_CRITICAL(&audio_lock);
    if (!current.active || current.starting) return;
    if (!current.network_enabled) {
        local_recordings_stats_t local;
        local_recordings_get_stats(&local);
        if (current.ending || !local.active) finish_session(current.ending ? "silence" : "local_complete");
        return;
    }
    if (sending_id != current.id) {
        const uint32_t prebuffer_frames = (uint32_t)(current.live_frame - current.first_frame);
        if (!backend_client_start_stream(current.id, prebuffer_frames * AUDIO_FRAME_MS,
                prebuffer_frames * AUDIO_FRAME_SAMPLES, current.detected_ms)) {
            disable_upload("network_error");
            return;
        }
        sending_id = current.id;
        cursor = current.first_frame;
        device_diagnostics_event(ESP_LOG_INFO, "KWS stream %lu started; sending until ambient noise", (unsigned long)sending_id);
    }
    for (unsigned batch = 0; batch < 8; ++batch) {
        portENTER_CRITICAL(&audio_lock);
        const bool ending = session.ending;
        const uint64_t available = ending ? session.end_frame : audio_history.total_frames;
        const bool has_frame = cursor < available;
        const bool valid = has_frame && audio_history_copy(&audio_history, cursor, samples);
        if (has_frame && !valid) stream_overflows++;
        portEXIT_CRITICAL(&audio_lock);
        if (!has_frame) {
            if (ending) finish_session("silence");
            return;
        }
        if (!valid) {
            disable_upload("buffer_overrun");
            return;
        }
        if (!backend_client_send_samples(samples, AUDIO_FRAME_SAMPLES, cursor < current.live_frame)) {
            disable_upload("network_error");
            return;
        }
        cursor++;
        websocket_frames_sent++;
    }
}

static void log_and_send_metrics(void) {
    kws_stats_t kws_stats;
    kws_detector_get_stats(&kws_stats);
    multi_heap_info_t heap, psram;
    heap_caps_get_info(&heap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    extern char _data_start[], _data_end[], _bss_start[], _bss_end[];
    extern char _ext_ram_bss_start[], _ext_ram_bss_end[];
    const size_t static_ram = (uintptr_t)_data_end - (uintptr_t)_data_start +
                              (uintptr_t)_bss_end - (uintptr_t)_bss_start;
    const size_t psram_static = (uintptr_t)_ext_ram_bss_end - (uintptr_t)_ext_ram_bss_start;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&audio_lock);
    const unsigned dma_lost = dma_overflows, queue_lost = kws_queue_drops;
    const unsigned stream_lost = stream_overflows, read_errors = capture_errors;
    const unsigned dropped = dropped_triggers;
    const uint64_t captured = audio_history.total_frames;
    const float noise = ambient.noise_rms, rms = last_rms;
    portEXIT_CRITICAL(&audio_lock);
    const uint64_t frame_delta = captured - metrics_previous_frames;
    const uint64_t sent_delta = websocket_frames_sent - metrics_previous_sent;
    const int64_t elapsed = now - metrics_previous_time_us;
    const float frame_rate = diagnostic_frame_rate(frame_delta, elapsed);
    const char *state = device_state_name(device_leds_get());
    local_recordings_stats_t local;
    local_recordings_get_stats(&local);
    playback_stats_t play;
    playback_buffer_get_stats(&play);
    const bool backend_ready = backend_client_is_ready();
    /* Main task only: reuse one bounded formatting buffer for text then JSON. */
    static char metrics[2048];
    snprintf(metrics, sizeof(metrics),
        "STATUS uptime=%llums state=%s backend=%s local=%s saved=%lu local_errors=%lu local_pcm=%luB\n"
        "RAM internal_heap used=%uB free=%uB min_free=%uB largest=%uB | static_data_bss=%uB | arena=%u/%uB (included in static)\n"
        "PSRAM total=%uB heap_used=%uB free=%uB min_free=%uB largest=%uB static_bss=%uB (excluded from heap)\n"
        "MIC frames=%llu (+%llu/%.2fs, %.1ffps) pcm_bytes=%llu | WS_sent=%llu (+%llu) | rms=%.1f ambient=%.1f\n"
        "KWS score=%.3f peak=%.3f rms100ms_peak=%.1fdBFS stride=%ums selftest=%s hits=%u inferences=%u avg=%uus/%ucycles heap_before=%uB heap_after=%uB\n"
        "HEALTH dma_overflows=%u kws_queue_drops=%u stream_overflows=%u read_errors=%u dropped_triggers=%u | stack_min_free capture=%uB kws=%uB recorder=%uB main=%uB speaker=%uB\n"
        "PLAY rx=%u submitted=%u queue=%u/%u peak=%u overflow=%u invalid=%u write_errors=%u underruns=%u rx_gap_max=%ums write_max=%ums\n",
        (unsigned long long)(now / 1000), state, backend_ready ? "READY" : "OFFLINE",
        local.status, (unsigned long)local.saved, (unsigned long)local.errors, (unsigned long)local.pcm_bytes,
        (unsigned)heap.total_allocated_bytes, (unsigned)heap.total_free_bytes,
        (unsigned)heap.minimum_free_bytes, (unsigned)heap.largest_free_block,
        (unsigned)static_ram, (unsigned)kws_stats.tensor_arena_used_bytes,
        (unsigned)kws_stats.tensor_arena_bytes,
        (unsigned)esp_psram_get_size(), (unsigned)psram.total_allocated_bytes,
        (unsigned)psram.total_free_bytes, (unsigned)psram.minimum_free_bytes,
        (unsigned)psram.largest_free_block, (unsigned)psram_static,
        (unsigned long long)captured, (unsigned long long)frame_delta, elapsed / 1000000.0,
        frame_rate, (unsigned long long)(captured * AUDIO_FRAME_SAMPLES * sizeof(int16_t)),
        (unsigned long long)websocket_frames_sent, (unsigned long long)sent_delta,
        rms, noise, kws_stats.last_score, kws_stats.peak_score, kws_stats.peak_rms_100ms_dbfs,
        (unsigned)kws_stats.inference_interval_ms, kws_stats.selftest_passed ? "PASS" : "FAIL",
        (unsigned)kws_stats.keyword_hits,
        (unsigned)kws_stats.inference_count, (unsigned)kws_stats.average_inference_us,
        (unsigned)kws_stats.average_inference_cycles, (unsigned)kws_stats.free_heap_before_bytes,
        (unsigned)kws_stats.free_heap_after_bytes, dma_lost, queue_lost, stream_lost, read_errors,
        dropped, (unsigned)uxTaskGetStackHighWaterMark(capture_handle),
        (unsigned)uxTaskGetStackHighWaterMark(kws_handle), (unsigned)local_recordings_stack_free(),
        (unsigned)uxTaskGetStackHighWaterMark(NULL),
        speaker_handle ? (unsigned)uxTaskGetStackHighWaterMark(speaker_handle) : 0,
        (unsigned)play.received, (unsigned)play.submitted, play.queued, PLAYBACK_QUEUE_FRAMES, play.high_water,
        (unsigned)play.overflows, (unsigned)play.invalid, (unsigned)play.write_errors, (unsigned)play.underruns,
        (unsigned)play.rx_gap_max_ms, (unsigned)play.write_max_ms);
    device_diagnostics_publish(metrics);
    ESP_LOGI(TAG, "%s", metrics);
    metrics_previous_frames = captured;
    metrics_previous_sent = websocket_frames_sent;
    metrics_previous_time_us = now;

    const int length = snprintf(
        metrics,
        sizeof(metrics),
        "{\"type\":\"metrics\",\"device_id\":\"%s\"," 
        "\"model\":\"hello_tors_int8\",\"uptime_ms\":%llu,\"state\":\"%s\",\"local_recordings_saved\":%lu,\"local_recording_errors\":%lu,"
        "\"model_flash_bytes\":%u,\"tensor_arena_bytes\":%u,"
        "\"tensor_arena_used_bytes\":%u,\"free_heap_before_bytes\":%u,"
        "\"free_heap_after_bytes\":%u,\"minimum_free_heap_bytes\":%u,"
        "\"inference_count\":%u,"
        "\"inference_cycles_avg\":%u,\"inference_us_avg\":%u,"
        "\"keyword_hits\":%u,\"false_activation_count\":%u,\"last_score\":%.4f,"
        "\"peak_score\":%.4f,\"peak_rms_100ms_dbfs\":%.1f,\"inference_interval_ms\":%u,\"kws_selftest_passed\":%s,"
        "\"dma_overflows\":%u,\"kws_queue_drops\":%u,\"stream_overflows\":%u,"
        "\"dropped_trigger_count\":%u,\"ambient_rms\":%.1f,\"audio_rms\":%.1f,"
        "\"internal_heap_used_bytes\":%u,\"internal_heap_free_bytes\":%u,"
        "\"internal_heap_min_free_bytes\":%u,\"internal_heap_largest_block_bytes\":%u,"
        "\"psram_total_bytes\":%u,\"psram_heap_used_bytes\":%u,\"psram_heap_free_bytes\":%u,"
        "\"psram_heap_min_free_bytes\":%u,\"psram_heap_largest_block_bytes\":%u,\"psram_static_bss_bytes\":%u,"
        "\"static_data_bss_bytes\":%u,\"mic_frames_captured\":%llu,\"mic_pcm_bytes\":%llu,"
        "\"mic_frames_per_second\":%.2f,\"ws_audio_frames_sent\":%llu,\"read_errors\":%u,"
        "\"playback_rx_frames\":%u,\"playback_submitted_frames\":%u,\"playback_queue_frames\":%u,"
        "\"playback_overflows\":%u,\"playback_invalid\":%u,\"playback_write_errors\":%u,\"playback_underruns\":%u,"
        "\"playback_rx_gap_max_ms\":%u,\"playback_write_max_ms\":%u}",
        DEVICE_ID,
        (unsigned long long)(esp_timer_get_time() / 1000),
        state, (unsigned long)local.saved, (unsigned long)local.errors,
        (unsigned)kws_stats.model_flash_bytes,
        (unsigned)kws_stats.tensor_arena_bytes,
        (unsigned)kws_stats.tensor_arena_used_bytes,
        (unsigned)kws_stats.free_heap_before_bytes,
        (unsigned)kws_stats.free_heap_after_bytes,
        (unsigned)heap.minimum_free_bytes,
        (unsigned)kws_stats.inference_count,
        (unsigned)kws_stats.average_inference_cycles,
        (unsigned)kws_stats.average_inference_us,
        (unsigned)kws_stats.keyword_hits,
        (unsigned)kws_stats.false_activation_count,
        kws_stats.last_score, kws_stats.peak_score, kws_stats.peak_rms_100ms_dbfs,
        (unsigned)kws_stats.inference_interval_ms, kws_stats.selftest_passed ? "true" : "false",
        dma_lost, queue_lost, stream_lost, dropped, noise, rms,
        (unsigned)heap.total_allocated_bytes, (unsigned)heap.total_free_bytes,
        (unsigned)heap.minimum_free_bytes, (unsigned)heap.largest_free_block,
        (unsigned)esp_psram_get_size(), (unsigned)psram.total_allocated_bytes,
        (unsigned)psram.total_free_bytes, (unsigned)psram.minimum_free_bytes,
        (unsigned)psram.largest_free_block, (unsigned)psram_static,
        (unsigned)static_ram, (unsigned long long)captured,
        (unsigned long long)(captured * AUDIO_FRAME_SAMPLES * sizeof(int16_t)),
        frame_rate, (unsigned long long)websocket_frames_sent, read_errors,
        (unsigned)play.received, (unsigned)play.submitted, play.queued, (unsigned)play.overflows,
        (unsigned)play.invalid, (unsigned)play.write_errors, (unsigned)play.underruns,
        (unsigned)play.rx_gap_max_ms, (unsigned)play.write_max_ms);
    if (length > 0 && length < (int)sizeof(metrics)) {
        backend_client_send_metrics(metrics);
    } else {
        device_diagnostics_event(ESP_LOG_WARN, "Backend metrics exceeded formatting buffer");
    }
}

static void backend_init_task(void *argument) {
    (void)argument;
    const esp_err_t error = backend_client_init();
    if (error != ESP_OK) {
        device_diagnostics_event(ESP_LOG_WARN, "Backend initialization failed: %s", esp_err_to_name(error));
    }
    vTaskDelete(NULL);
}

static void speaker_playback_task(void *argument) {
    (void)argument;
    playback_frame_t frame;
    int32_t tx_samples[320];
    uint32_t active_id = 0, active_generation = 0;
    bool have_session = false;

    while (true) {
        if (!playback_buffer_read(&frame, pdMS_TO_TICKS(SPEAKER_IDLE_WAIT_MS))) {
            /* This wait already exceeds the whole TX ring duration. */
            if (speaker_tx_enabled) {
                if (i2s_channel_disable(i2s_tx_handle) == ESP_OK) speaker_tx_enabled = false;
                else playback_buffer_written(active_generation, false);
            }
            if (have_session && playback_buffer_current(active_generation)) {
                /* LAST may have committed just after the queue read timed out. */
                if (playback_buffer_drained(active_generation)) {
                    backend_client_queue_playback_stop(active_id, active_generation,
                        !playback_buffer_failed(active_generation));
                    have_session = false;
                } else if (atomic_load(&playback_active)) {
                    playback_buffer_underrun();
                }
            }
            atomic_store(&playback_active, false);
            /* Also yield if backend initialization failed before creating the queue. */
            vTaskDelay(1);
            continue;
        }
        if (!playback_buffer_current(frame.generation)) continue;
        if (have_session && active_generation != frame.generation && speaker_tx_enabled) {
            if (i2s_channel_disable(i2s_tx_handle) == ESP_OK) speaker_tx_enabled = false;
            else { playback_buffer_written(frame.generation, false); continue; }
        }
        have_session = true;
        active_id = frame.audio_id;
        active_generation = frame.generation;
        atomic_store(&playback_active, true);

        if (!speaker_tx_enabled) {
            /* Hold frame zero, accumulate four more; never re-gate steady playback.
             * LAST/play_stop releases short clips. Limit waiting for a stalled sender. */
            const int64_t prebuffer_deadline = esp_timer_get_time() + PLAYBACK_PREBUFFER_WAIT_MS * 1000LL;
            while (playback_buffer_current(frame.generation) &&
                   !playback_buffer_ready(frame.generation, 1) &&
                   esp_timer_get_time() < prebuffer_deadline) {
                vTaskDelay(1);
            }
            if (!playback_buffer_current(frame.generation)) {
                have_session = false;
                atomic_store(&playback_active, false);
                continue;
            }
            if (!playback_buffer_ready(frame.generation, 1)) {
                device_diagnostics_event(ESP_LOG_WARN, "Speaker prebuffer wait reached %ums; starting available audio",
                                         PLAYBACK_PREBUFFER_WAIT_MS);
            }
            /* Clear the stopped ring, including stale audio from an interrupted clip. */
            memset(tx_samples, 0, sizeof(tx_samples));
            bool cleared = true;
            for (unsigned i = 0; i < I2S_DMA_DESCRIPTORS; ++i) {
                size_t loaded = 0;
                if (i2s_channel_preload_data(i2s_tx_handle, tx_samples, sizeof(tx_samples), &loaded) != ESP_OK ||
                    loaded != sizeof(tx_samples)) { cleared = false; break; }
            }
            if (!cleared) { playback_buffer_written(frame.generation, false); continue; }
            if (i2s_channel_enable(i2s_tx_handle) != ESP_OK) {
                playback_buffer_written(frame.generation, false);
                continue;
            }
            speaker_tx_enabled = true;
        }

        for (size_t i = 0; i < 320; ++i) {
            tx_samples[i] = speaker_pcm_to_i2s(frame.samples[i]);
        }

        size_t bytes_written = 0;
        const int64_t write_start_us = esp_timer_get_time();
        const esp_err_t error = i2s_channel_write(
            i2s_tx_handle,
            tx_samples,
            sizeof(tx_samples),
            &bytes_written,
            1000);
        playback_buffer_write_timing((uint32_t)((esp_timer_get_time() - write_start_us + 999) / 1000));
        playback_buffer_written(frame.generation, error == ESP_OK && bytes_written == sizeof(tx_samples));
        if (frame.last) {
            /* write() queues DMA data; it does not mean the amplifier has heard it. */
            vTaskDelay(pdMS_TO_TICKS(SPEAKER_DRAIN_MS));
            if (i2s_channel_disable(i2s_tx_handle) == ESP_OK) speaker_tx_enabled = false;
            else playback_buffer_written(frame.generation, false);
            if (playback_buffer_current(frame.generation))
                backend_client_queue_playback_stop(frame.audio_id, frame.generation,
                    !playback_buffer_failed(frame.generation));
            have_session = false;
            atomic_store(&playback_active, false);
        }
    }
}

static esp_err_t play_boot_audio(void) {
    float phase = 0.0f;

    ESP_LOGI(TAG, "Playing two startup beeps");

    for (size_t note_index = 0;
         note_index < sizeof(boot_beeps) / sizeof(boot_beeps[0]);
         ++note_index) {
        const melody_note_t note = boot_beeps[note_index];
        const size_t note_samples = ((size_t)SAMPLE_RATE * note.duration_ms) / 1000;
        size_t samples_generated = 0;

        while (samples_generated < note_samples) {
            const size_t samples_remaining = note_samples - samples_generated;
            const size_t chunk_samples = samples_remaining < AUDIO_TX_BUFFER_SAMPLES
                                             ? samples_remaining
                                             : AUDIO_TX_BUFFER_SAMPLES;

            for (size_t i = 0; i < chunk_samples; ++i) {
                const size_t sample_index = samples_generated + i;
                float envelope = 1.0f;
                const size_t edge_samples = SAMPLE_RATE / 100;

                if (edge_samples > 0 && sample_index < edge_samples) {
                    envelope = (float)sample_index / (float)edge_samples;
                } else if (edge_samples > 0 && sample_index + edge_samples > note_samples) {
                    envelope = (float)(note_samples - sample_index) / (float)edge_samples;
                }

                const float sample = note.frequency_hz == 0
                                         ? 0.0f
                                         : sinf(phase) * MELODY_AMPLITUDE * envelope;
                audio_tx_buffer[i] = speaker_pcm_to_i2s((int16_t)sample);

                phase += TWO_PI * (float)note.frequency_hz / (float)SAMPLE_RATE;
                if (phase >= TWO_PI) {
                    phase -= TWO_PI;
                }
            }

            size_t bytes_written = 0;
            const esp_err_t error = i2s_channel_write(
                i2s_tx_handle,
                audio_tx_buffer,
                chunk_samples * sizeof(audio_tx_buffer[0]),
                &bytes_written,
                1000);
            if (error != ESP_OK || bytes_written != chunk_samples * sizeof(audio_tx_buffer[0])) {
                return error != ESP_OK ? error : ESP_FAIL;
            }

            samples_generated += chunk_samples;
        }
    }

    // Allow the final DMA buffer to reach the amplifier, then stop TX so the
    // MAX98357A cannot replay stale data after the song.
    vTaskDelay(pdMS_TO_TICKS(SPEAKER_DRAIN_MS));
    const esp_err_t disable_error = i2s_channel_disable(i2s_tx_handle);
    if (disable_error != ESP_OK) {
        return disable_error;
    }
    speaker_tx_enabled = false;

    ESP_LOGI(TAG, "Startup beeps finished");
    return ESP_OK;
}

static void start_recording(void) {
    recording = true;
    portENTER_CRITICAL(&audio_lock);
    usb_frame_cursor = audio_history.total_frames;
    portEXIT_CRITICAL(&audio_lock);
    send_recorder_text("START\n");
}

static void stop_recording(void) {
    recording = false;
    send_recorder_text("STOP\n");
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
}

static void service_usb_recording(void) {
    if (!recording) return;
    int16_t samples[AUDIO_FRAME_SAMPLES];
    for (unsigned n = 0; n < 4; ++n) {
        portENTER_CRITICAL(&audio_lock);
        const bool pending = usb_frame_cursor < audio_history.total_frames;
        const bool valid = pending && audio_history_copy(&audio_history, usb_frame_cursor, samples);
        portEXIT_CRITICAL(&audio_lock);
        if (!pending) return;
        if (!valid || !send_recorder_bytes(samples, sizeof(samples))) {
            ESP_LOGW(TAG, "USB recorder too slow/disconnected; stopping USB recording");
            stop_recording();
            return;
        }
        usb_frame_cursor++;
    }
}

void app_main(void) {
    esp_err_t error = init_gpio();
    if (error != ESP_OK) {
        fatal_error("GPIO initialization", error);
    }

    // External BSS requires PSRAM; IDF already fails boot if init/memtest fails.
    if (!esp_psram_is_initialized()) {
        fatal_error("N16R8 PSRAM initialization", ESP_ERR_INVALID_STATE);
    }
    ESP_LOGI(TAG, "PSRAM detected=%uB; audio history=%uB external; model arena/DMA/stacks internal",
             (unsigned)esp_psram_get_size(), (unsigned)sizeof(audio_history));

    error = init_recorder_transport();
    if (error != ESP_OK) {
        fatal_error("USB recorder transport initialization", error);
    }

    error = init_i2s();
    if (error != ESP_OK) {
        fatal_error("I2S initialization", error);
    }

    if (!kws_detector_init()) {
        fatal_error("Hello Tors KWS initialization", ESP_FAIL);
    }

    // No formatting on failure: preserve unrecognized storage and keep KWS/network usable.
    local_recordings_init(copy_recording_frame);

    if (xTaskCreate(backend_init_task, "backend_init", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Could not start backend initialization task");
    }
    error = play_boot_audio();
    if (error != ESP_OK) ESP_LOGW(TAG, "Startup beeps failed: %s", esp_err_to_name(error));

    kws_queue = xQueueCreate(KWS_QUEUE_FRAMES, sizeof(kws_frame_t));
    if (!kws_queue) fatal_error("KWS queue", ESP_ERR_NO_MEM);
    if (xTaskCreatePinnedToCore(inference_task, "kws", 6144, NULL, 2, &kws_handle, 1) != pdPASS)
        fatal_error("KWS task", ESP_ERR_NO_MEM);
    if (xTaskCreatePinnedToCore(capture_task, "mic_capture", 4096, NULL, 8, &capture_handle, 0) != pdPASS)
        fatal_error("Capture task", ESP_ERR_NO_MEM);
    if (xTaskCreate(speaker_playback_task, "speaker_playback", 4096, NULL, 4, &speaker_handle) != pdPASS) {
        ESP_LOGW(TAG, "Could not start speaker playback task");
    }

    device_diagnostics_event(ESP_LOG_INFO, "Keep quiet for two seconds while ambient noise is calibrated");
    ESP_LOGI(TAG, "Diagnostics every 5s: MIC frame=20ms/320 samples/640 PCM bytes; expected=50fps. Heap excludes static RAM.");
    send_recorder_text("READY\n");
    metrics_previous_time_us = esp_timer_get_time();
    next_metrics_time_us = metrics_previous_time_us + ((int64_t)METRICS_PERIOD_MS * 1000);

    int64_t last_button_press_us = 0;
    while (true) {
        const int button_state = gpio_get_level(BUTTON_PIN);

        const int64_t now = esp_timer_get_time();
        if (last_button_state == 1 && button_state == 0 &&
            now - last_button_press_us >= BUTTON_DEBOUNCE_MS * 1000) {
            last_button_press_us = now;
            if (recording) {
                stop_recording();
            } else {
                start_recording();
            }
        }

        last_button_state = button_state;

        service_stream();
        service_usb_recording();
        backend_client_service_playback();

        if (esp_timer_get_time() >= next_metrics_time_us) {
            log_and_send_metrics();
            next_metrics_time_us = esp_timer_get_time() + ((int64_t)METRICS_PERIOD_MS * 1000);
        }

        /* Keep Wi-Fi, USB, and the idle task scheduled between audio blocks. */
        vTaskDelay(1);
    }
}
