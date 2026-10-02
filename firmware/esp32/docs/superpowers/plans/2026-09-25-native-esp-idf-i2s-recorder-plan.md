# Native ESP-IDF I2S Recorder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the supplied Arduino I2S microphone recorder into the existing ESP-IDF project using native ESP-IDF APIs.

**Architecture:** Keep one application source file, `main/main.c`, because the current project contains only an empty ESP-IDF entry point. Configure ESP32-S3 GPIOs and one I2S RX channel in `app_main()`, poll the active-low button, convert 32-bit I2S samples to 16-bit PCM, and stream PCM bytes through UART0.

**Tech Stack:** ESP-IDF 5.5.2, ESP32-S3, C17, FreeRTOS, `driver/gpio.h`, `driver/i2s_std.h`, `driver/uart.h`, and ESP-IDF logging.

---

### Task 1: Add a host-testable PCM conversion helper

**Files:**
- Create: `tests/pcm_conversion_test.c`
- Create: `main/audio_conversion.c`
- Create: `main/audio_conversion.h`
- Modify: `main/main.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/pcm_conversion_test.c`:

```c
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

void convert_i2s_to_pcm(const int32_t *input, int16_t *output, size_t sample_count);

int main(void) {
    const int32_t input[] = {0x00010000, 0x7fff0000, (int32_t)0xffff0000, 0x00007fff};
    int16_t output[4] = {0};

    convert_i2s_to_pcm(input, output, 4);

    assert(output[0] == 1);
    assert(output[1] == 32767);
    assert(output[2] == -1);
    assert(output[3] == 0);
    puts("pcm conversion test passed");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run from the project root:

```powershell
New-Item -ItemType Directory -Force tests\build | Out-Null
gcc tests\pcm_conversion_test.c main\audio_conversion.c -o tests\build\pcm_conversion_test.exe
```

Expected result: compilation or link failure because `convert_i2s_to_pcm` does not exist yet.

- [ ] **Step 3: Add the minimal conversion implementation**

Implement the helper in `main/audio_conversion.c` and declare it in `main/audio_conversion.h`:

```c
void convert_i2s_to_pcm(const int32_t *input, int16_t *output, size_t sample_count) {
    for (size_t i = 0; i < sample_count; ++i) {
        output[i] = (int16_t)(input[i] >> 16);
    }
}
```

The conversion helper is deliberately isolated so it can be tested with the host compiler without pulling ESP-IDF hardware headers into the test.

- [ ] **Step 4: Run the test to verify it passes**

Run:

```powershell
gcc tests\pcm_conversion_test.c main\audio_conversion.c -o tests\build\pcm_conversion_test.exe
.\tests\build\pcm_conversion_test.exe
```

Expected output:

```text
pcm conversion test passed
```

- [ ] **Step 5: Keep the project component declaration valid**

Ensure `main/CMakeLists.txt` remains:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
)
```

Do not add Arduino libraries or external dependencies.

### Task 2: Implement native ESP-IDF GPIO and I2S behavior

**Files:**
- Modify: `main/main.c`

- [ ] **Step 1: Add the approved pin and audio constants**

Use these definitions:

```c
#define I2S_BCLK_PIN GPIO_NUM_4
#define I2S_WS_PIN GPIO_NUM_5
#define I2S_DATA_PIN GPIO_NUM_6
#define RED_LED_PIN GPIO_NUM_7
#define BLUE_LED_PIN GPIO_NUM_15
#define BUTTON_PIN GPIO_NUM_16
#define SAMPLE_RATE 16000
#define AUDIO_BUFFER_SAMPLES 256
```

- [ ] **Step 2: Implement GPIO initialization**

Use `gpio_config_t` to configure GPIO 7 and GPIO 15 as outputs initialized LOW, and GPIO 16 as an input with pull-up enabled. Initialize `last_button_state` to `1` to preserve the Arduino active-low edge detector.

- [ ] **Step 3: Implement I2S channel initialization**

Use the ESP-IDF 5.5 standard I2S API:

```c
i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
i2s_new_channel(&channel_config, NULL, &rx_handle);

i2s_std_config_t std_config = {
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

i2s_channel_init_std_mode(rx_handle, &std_config);
i2s_channel_enable(rx_handle);
```

Check every ESP-IDF return value with `ESP_ERROR_CHECK` or an explicit fatal-error path that logs the error and enters the red LED blink loop.

- [ ] **Step 4: Implement recording state transitions**

Implement `start_recording()` and `stop_recording()` so their GPIO, state, and serial output match the approved design:

```c
recording = true;
gpio_set_level(RED_LED_PIN, 1);
gpio_set_level(BLUE_LED_PIN, 0);
printf("START\n");
```

```c
recording = false;
printf("STOP\n");
gpio_set_level(RED_LED_PIN, 0);
gpio_set_level(BLUE_LED_PIN, 1);
```

- [ ] **Step 5: Implement the fatal I2S error loop**

Log the initialization failure, then blink the red LED every 200 ms using `vTaskDelay(pdMS_TO_TICKS(200))`. Do not attempt audio reads after entering this loop.

### Task 3: Implement PCM capture and button-driven main loop

**Files:**
- Modify: `main/main.c`

- [ ] **Step 1: Define capture-loop expectations**

The capture path must read at most `AUDIO_BUFFER_SAMPLES * sizeof(int32_t)` bytes per iteration, ignore zero-byte reads, compute the sample count from the returned byte count, convert only returned samples, and write exactly `sample_count * sizeof(int16_t)` bytes to UART0.

- [ ] **Step 2: Implement the capture function**

Use `i2s_channel_read()` with a finite timeout. Convert samples with the helper, then call:

```c
uart_write_bytes(UART_NUM_0, (const char *)pcm_buffer, sample_count * sizeof(int16_t));
```

- [ ] **Step 3: Implement the button edge detector**

In the main FreeRTOS loop, use:

```c
int button_state = gpio_get_level(BUTTON_PIN);
if (last_button_state == 1 && button_state == 0) {
    if (recording) {
        stop_recording();
    } else {
        start_recording();
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}
last_button_state = button_state;
```

When `recording` is true, call the capture function. Add a short loop delay when not recording so the CPU does not busy-spin.

- [ ] **Step 4: Run the host conversion test again**

Run:

```powershell
.\tests\build\pcm_conversion_test.exe
```

Expected output: `pcm conversion test passed`.

### Task 4: Build and validate the ESP-IDF application

**Files:**
- Modify: `main/main.c` if build errors require API corrections.
- Modify: `main/CMakeLists.txt` only if required by the final source layout.

- [ ] **Step 1: Build for ESP32-S3**

Run with the installed ESP-IDF toolchain:

```powershell
$env:IDF_PATH='C:\esp\v5.5.2\esp-idf'
$env:IDF_TOOLS_PATH='C:\Espressif\tools'
& 'C:\Espressif\tools\python\v5.5.2\venv\Scripts\python.exe' 'C:\esp\v5.5.2\esp-idf\tools\idf.py' set-target esp32s3 build
```

Expected result: the build completes and produces the `EdgeAIKWS` application binary without compiler errors.

- [ ] **Step 2: Inspect the generated application configuration**

Confirm the generated build still reports target `esp32s3`, console UART0, and monitor baud 115200.

- [ ] **Step 3: Review the final file set**

Run:

```powershell
Get-ChildItem -Recurse -File docs\superpowers, main, tests | Select-Object FullName
```

Confirm that only the approved native ESP-IDF recorder, its focused conversion test, and the design/plan documents were added or changed. Do not add Arduino headers or Arduino-specific APIs.
