# Native ESP-IDF I2S Recorder Design

## Goal

Port the provided Arduino I2S microphone recorder behavior into the existing ESP-IDF `EdgeAIKWS` project using native ESP-IDF APIs.

## Scope

The first version will implement only local capture and serial streaming:

- ESP32-S3 target.
- Standard I2S receive mode.
- BCLK on GPIO 4.
- WS/LRCLK on GPIO 5.
- Microphone data on GPIO 6.
- Red LED on GPIO 7.
- Blue LED on GPIO 15.
- Active-low push button on GPIO 16 with an internal pull-up.
- 16 kHz, mono, 32-bit I2S input.
- Conversion from 32-bit I2S samples to signed 16-bit PCM by shifting each sample right by 16 bits.
- PCM output through UART0 at 115200 baud.

## Runtime behavior

At boot, the application configures both LEDs, the button, UART0, and I2S. If I2S initialization succeeds, the red LED is turned on and `READY` is printed.

The main loop polls the button. A HIGH-to-LOW transition toggles recording state and applies a 200 ms debounce delay:

- Recording start: red LED on, blue LED off, print `START`.
- Recording stop: red LED off, blue LED on, print `STOP`.

While recording, the application reads I2S data, converts available samples to 16-bit PCM, and writes the PCM bytes to UART0.

If I2S initialization fails, the application never enters normal operation and blinks the red LED every 200 ms.

## Architecture

`main/main.c` remains the only application source file for this first increment. It will contain:

- GPIO initialization.
- I2S channel creation and standard-mode configuration.
- Recording state and button edge detection.
- Sample conversion and UART output.
- The main FreeRTOS task loop through `app_main()`.

The implementation will use ESP-IDF 5.5 APIs:

- `driver/gpio.h` for GPIO and LED/button control.
- `driver/i2s_std.h` and `driver/i2s_common.h` for I2S.
- `freertos/FreeRTOS.h` and `freertos/task.h` for delays.
- `driver/uart.h` or standard output for serial output.

## Data flow

```text
I2S microphone
    -> ESP32-S3 I2S RX DMA
    -> int32_t sample buffer
    -> right-shift by 16 bits
    -> int16_t PCM buffer
    -> UART0 byte stream
```

## Error handling

- A failed I2S channel allocation, configuration, or enable operation is treated as a fatal initialization error.
- Fatal initialization errors cause a red LED blink loop.
- A short or empty I2S read is ignored and the main loop continues.
- GPIO and I2S errors will be logged before entering the failure loop.

## Serial-stream constraint

`READY`, `START`, and `STOP` are human-readable text sent over the same UART as binary PCM, matching the supplied Arduino behavior. These messages can corrupt a raw PCM capture if the PC treats every received byte as audio. A later revision can move status messages to a separate channel or define a framed serial protocol.

## Validation

- Build the ESP-IDF project for ESP32-S3.
- Confirm the application source compiles against ESP-IDF 5.5.2.
- Inspect the generated firmware configuration for the expected target and console UART.
- Hardware validation will require an I2S microphone connected to GPIO 4, GPIO 5, GPIO 6, and a common ground, plus LEDs and a button wired to the specified GPIOs.
