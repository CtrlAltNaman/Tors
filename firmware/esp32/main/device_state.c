#include "device_state.h"
device_state_t device_state_resolve(device_inputs_t in) {
    if (in.fault) return DEVICE_ERROR;
    if (in.trigger) return DEVICE_TRIGGERED;
    if (in.recording) return DEVICE_RECORDING;
    if (in.playback) return DEVICE_PLAYBACK;
    if (in.preparing) return DEVICE_PREPARING;
    if (!in.calibrated) return DEVICE_CALIBRATING;
    return in.backend_ready ? DEVICE_LISTENING : DEVICE_OFFLINE;
}
led_pattern_t device_state_pattern(device_state_t state, uint32_t ms) {
    led_pattern_t p = {0};
    switch (state) {
    case DEVICE_BOOT: case DEVICE_CALIBRATING: case DEVICE_PREPARING:
        p.green = ms % 1000 < 500; break;
    case DEVICE_LISTENING: p.green = true; break;
    case DEVICE_OFFLINE: p.green = ms % 2000 < 100 || (ms % 2000 >= 200 && ms % 2000 < 300); break;
    case DEVICE_TRIGGERED: p.red = true; break;
    case DEVICE_RECORDING: p.red = ms % 500 < 250; break;
    case DEVICE_PLAYBACK: p.green = ms % 200 < 100; break;
    default: p.red = ms % 200 < 100; break;
    }
    return p;
}
const char *device_state_name(device_state_t s) {
    static const char *const names[] = {"BOOT","CALIBRATING","PREPARING","LISTENING",
        "OFFLINE","TRIGGERED","RECORDING","PLAYBACK","ERROR"};
    return s >= 0 && s < DEVICE_STATE_COUNT ? names[s] : "ERROR";
}
