#pragma once
#include "device_state.h"
#include "esp_err.h"
esp_err_t device_leds_init(void);
void device_leds_set(device_state_t state);
device_state_t device_leds_get(void);
