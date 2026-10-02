// Microphone capture at 16 kHz mono via the ESP-IDF I2S driver (std or PDM, see kws_config.h).
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t kws_mic_init(void);
// Blocks until n samples (n <= KWS_FRAME_STEP) are read; writes floats in [-1, 1).
esp_err_t kws_mic_read(float *out, int n);

#ifdef __cplusplus
}
#endif
