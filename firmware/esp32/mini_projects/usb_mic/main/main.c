#include <inttypes.h>
#include "driver/i2s_std.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_protocol.h"

#define MIC_BCLK GPIO_NUM_4
#define MIC_WS GPIO_NUM_5
#define MIC_DATA GPIO_NUM_6

static const char *TAG = "usb_mic";
static int32_t samples[MIC_FRAME_SAMPLES];
static uint8_t packet[MIC_PACKET_BYTES];
static portMUX_TYPE counter_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t dma_overflows;

static bool IRAM_ATTR on_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event, void *context) {
    (void)channel; (void)event; (void)context;
    portENTER_CRITICAL_ISR(&counter_lock);
    dma_overflows++;
    portEXIT_CRITICAL_ISR(&counter_lock);
    return false;
}

void app_main(void) {
    usb_serial_jtag_driver_config_t usb_config = {.tx_buffer_size = 4096, .rx_buffer_size = 64};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));

    i2s_chan_handle_t mic;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 8;
    channel.dma_frame_num = MIC_FRAME_SAMPLES;
    ESP_ERROR_CHECK(i2s_new_channel(&channel, NULL, &mic));
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = MIC_BCLK, .ws = MIC_WS,
                     .dout = I2S_GPIO_UNUSED, .din = MIC_DATA},
    };
    config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic, &config));
    const i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = on_overflow};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(mic, &callbacks, NULL));
    ESP_ERROR_CHECK(i2s_channel_enable(mic));
    ESP_LOGI(TAG, "Auto streaming: 16kHz mono PCM16; native USB audio, UART diagnostics only");

    uint32_t sequence = 0;
    uint64_t captured = 0, sent = 0, dropped = 0;
    uint32_t read_errors = 0;
    size_t filled = 0;
    int64_t next_log = esp_timer_get_time() + 5000000;
    for (;;) {
        size_t received = 0;
        esp_err_t error = i2s_channel_read(mic, (uint8_t *)samples + filled,
                                          sizeof(samples) - filled, &received, 100);
        if (error != ESP_OK && error != ESP_ERR_TIMEOUT) {
            read_errors++;
            filled = 0;
            vTaskDelay(1);
        } else {
            filled += received;
            if (filled == sizeof(samples)) {
                filled = 0;
                captured++;
                mic_encode(packet, sequence++, samples);
                /* IDF queues this packet atomically or returns 0. Never wait for a host. */
                if (usb_serial_jtag_write_bytes(packet, sizeof(packet), 0) == sizeof(packet)) sent++;
                else dropped++;
            }
        }
        if (esp_timer_get_time() >= next_log) {
            portENTER_CRITICAL(&counter_lock);
            const uint32_t overflows = dma_overflows;
            portEXIT_CRITICAL(&counter_lock);
            multi_heap_info_t heap;
            heap_caps_get_info(&heap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            ESP_LOGI(TAG, "MIC captured=%" PRIu64 " usb_queued=%" PRIu64 " usb_dropped=%" PRIu64
                     " dma_overflows=%" PRIu32 " read_errors=%" PRIu32,
                     captured, sent, dropped, overflows, read_errors);
            ESP_LOGI(TAG, "RAM heap_used=%uB free=%uB min_free=%uB largest=%uB stack_min_free=%uB",
                     (unsigned)heap.total_allocated_bytes, (unsigned)heap.total_free_bytes,
                     (unsigned)heap.minimum_free_bytes, (unsigned)heap.largest_free_block,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            next_log = esp_timer_get_time() + 5000000;
        }
    }
}
