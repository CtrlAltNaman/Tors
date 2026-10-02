#include "kws_mic.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "kws_config.h"
#include "kws_features.h"

#if KWS_MIC_TYPE == KWS_MIC_TYPE_PDM
#include "driver/i2s_pdm.h"
typedef int16_t sample_t;  // PDM RX: hardware PDM->PCM filter outputs 16-bit
#define SAMPLE_FULL_SCALE 32768.0f
#else
#include "driver/i2s_std.h"
typedef int32_t sample_t;  // I2S MEMS mics: 24-bit data, MSB-aligned in 32-bit slots
#define SAMPLE_FULL_SCALE 2147483648.0f
#endif

static i2s_chan_handle_t s_rx;

esp_err_t kws_mic_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;               // 8 x 20 ms = 160 ms of slack for inference jitter
    chan_cfg.dma_frame_num = KWS_FRAME_STEP;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) return err;

#if KWS_MIC_TYPE == KWS_MIC_TYPE_PDM
    i2s_pdm_rx_config_t cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(KWS_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = KWS_PDM_CLK_GPIO,
            .din = KWS_PDM_DIN_GPIO,
            .invert_flags = {.clk_inv = false},
        },
    };
    err = i2s_channel_init_pdm_rx_mode(s_rx, &cfg);
#else
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(KWS_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = KWS_I2S_BCLK_GPIO,
            .ws = KWS_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = KWS_I2S_DIN_GPIO,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    cfg.slot_cfg.slot_mask = KWS_I2S_USE_RIGHT_SLOT ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
    err = i2s_channel_init_std_mode(s_rx, &cfg);
#endif
    if (err != ESP_OK) return err;
    return i2s_channel_enable(s_rx);
}

esp_err_t kws_mic_read(float *out, int n) {
    sample_t buf[KWS_FRAME_STEP];
    if (n > KWS_FRAME_STEP) n = KWS_FRAME_STEP;
    size_t got = 0;
    esp_err_t err = i2s_channel_read(s_rx, buf, n * sizeof(sample_t), &got, portMAX_DELAY);
    if (err != ESP_OK) return err;
    for (int i = 0; i < n; i++) out[i] = (float)buf[i] * (1.0f / SAMPLE_FULL_SCALE);
    return ESP_OK;
}
