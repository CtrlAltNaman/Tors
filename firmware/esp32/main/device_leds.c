#include "device_leds.h"
#include <stdatomic.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#define RED_LED_PIN GPIO_NUM_15
#define GREEN_LED_PIN GPIO_NUM_7
static atomic_int current_state = DEVICE_BOOT;
static atomic_bool fatal;
static esp_timer_handle_t timer;
void device_leds_set(device_state_t state) {
    if (state == DEVICE_ERROR) atomic_store(&fatal, true);
    atomic_store(&current_state, state);
}
device_state_t device_leds_get(void) {
    return atomic_load(&fatal) ? DEVICE_ERROR : atomic_load(&current_state);
}
static void tick(void *arg) {
    (void)arg;
    led_pattern_t p = device_state_pattern(device_leds_get(), (uint32_t)(esp_timer_get_time()/1000));
    // Sole GPIO writer after initialization. Break before make, never both on.
    gpio_set_level(RED_LED_PIN, 0);
    gpio_set_level(GREEN_LED_PIN, 0);
    if (p.red) gpio_set_level(RED_LED_PIN, 1);
    else if (p.green) gpio_set_level(GREEN_LED_PIN, 1);
}
esp_err_t device_leds_init(void) {
    gpio_config_t config = {.pin_bit_mask=(1ULL<<RED_LED_PIN)|(1ULL<<GREEN_LED_PIN),
                           .mode=GPIO_MODE_OUTPUT};
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;
    tick(NULL);
    const esp_timer_create_args_t args = {.callback=tick,.name="status_leds",.skip_unhandled_events=true};
    err = esp_timer_create(&args, &timer);
    if (err == ESP_OK) err = esp_timer_start_periodic(timer, 50000);
    return err;
}
