#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    DEVICE_BOOT, DEVICE_CALIBRATING, DEVICE_PREPARING, DEVICE_LISTENING,
    DEVICE_OFFLINE, DEVICE_TRIGGERED, DEVICE_RECORDING, DEVICE_PLAYBACK,
    DEVICE_ERROR, DEVICE_STATE_COUNT
} device_state_t;
typedef struct { bool red, green; } led_pattern_t;
typedef struct {
    bool fault, trigger, recording, playback, preparing, calibrated, backend_ready;
} device_inputs_t;
device_state_t device_state_resolve(device_inputs_t inputs);
led_pattern_t device_state_pattern(device_state_t state, uint32_t ms);
const char *device_state_name(device_state_t state);
