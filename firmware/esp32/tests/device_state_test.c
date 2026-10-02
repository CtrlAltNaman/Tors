#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "device_state.h"
int main(void) {
    for (int s = 0; s < DEVICE_STATE_COUNT; ++s)
        for (unsigned ms = 0; ms < 10000; ++ms) {
            led_pattern_t p = device_state_pattern(s, ms);
            assert(!(p.red && p.green));
            assert(strlen(device_state_name(s)) > 0);
        }
    device_inputs_t in = {.calibrated=true, .backend_ready=true};
    assert(device_state_resolve(in) == DEVICE_LISTENING);
    in.backend_ready=false;
    assert(device_state_resolve(in) == DEVICE_OFFLINE);
    in.recording=true;
    assert(device_state_resolve(in) == DEVICE_RECORDING);
    in.trigger=true;
    assert(device_state_resolve(in) == DEVICE_TRIGGERED);
    in.fault=true;
    assert(device_state_resolve(in) == DEVICE_ERROR);
    assert(device_state_pattern(DEVICE_TRIGGERED, 200).red);
    assert(device_state_pattern(DEVICE_LISTENING, 1200).green);
    assert(device_state_pattern(DEVICE_OFFLINE, 50).green);
    assert(!device_state_pattern(DEVICE_OFFLINE, 150).green);
    assert(device_state_pattern(DEVICE_OFFLINE, 250).green);
    assert(!device_state_pattern(DEVICE_OFFLINE, 800).green);
    puts("LED states: priority, patterns and mutual exclusion passed");
}
