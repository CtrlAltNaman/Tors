# ESP32 WebSocket Backend Streaming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add button-triggered Wi-Fi WebSocket PCM streaming from the ESP32-S3 to the existing backend at `ws://192.168.1.42:8765`.

**Architecture:** Keep I2S and GPIO control in `main/main.c`. Add a small backend transport module that owns Wi-Fi, WebSocket connection state, protocol JSON, 320-sample buffering, and 16-byte binary frame packing. Keep credentials in a local configuration header and use the official ESP-IDF WebSocket component.

**Tech Stack:** ESP-IDF 5.5.2, ESP32-S3, FreeRTOS, esp_wifi, esp_websocket_client, 16 kHz mono PCM s16le.

---

### Task 1: Lock the transport contract with host tests

**Files:**
- Modify: `tests/test_firmware_settings.py`

- [ ] **Step 1: Add assertions for the WebSocket component, endpoint, and frame constants.**

Assert that the firmware declares `esp_websocket_client`, uses `192.168.1.42:8765`, and defines 320 samples / 640 PCM bytes per frame.

- [ ] **Step 2: Run the focused test and confirm it fails against the current UART-only firmware.**

Run: `python tests\test_firmware_settings.py`

Expected: FAIL because the current firmware has no WebSocket transport or frame constants.

### Task 2: Add local network configuration and the ESP-IDF dependency

**Files:**
- Create: `main/network_config.h`
- Create: `main/idf_component.yml`
- Modify: `main/CMakeLists.txt`
- Modify: `.gitignore`

- [ ] **Step 1: Add local values for Wi-Fi and the backend URI.**

Define `WIFI_SSID`, `WIFI_PASSWORD`, and `BACKEND_URI` in `main/network_config.h`. Keep this file local because it contains the supplied Wi-Fi password.

- [ ] **Step 2: Declare `espressif/esp_websocket_client` in `main/idf_component.yml`.**

- [ ] **Step 3: Add `esp_wifi`, `esp_event`, `esp_netif`, `nvs_flash`, and `espressif/esp_websocket_client` to the main component requirements.**

- [ ] **Step 4: Ignore the local credentials header.**

- [ ] **Step 5: Run the focused test and confirm dependency/config assertions still fail only on the missing implementation.**

### Task 3: Implement Wi-Fi and WebSocket transport

**Files:**
- Create: `main/backend_client.h`
- Create: `main/backend_client.c`

- [ ] **Step 1: Write the failing implementation seam tests.**

Expose `backend_client_init`, `backend_client_is_ready`, `backend_client_start_stream`, `backend_client_send_samples`, and `backend_client_stop_stream` through the header.

- [ ] **Step 2: Implement Wi-Fi station setup and reconnect handling.**

Initialize NVS, TCP/IP, the default event loop, Wi-Fi station mode, and connection retries. Log failures through the existing ESP-IDF UART console.

- [ ] **Step 3: Implement WebSocket connection setup.**

On connection, send the frozen `hello` JSON. Track connection state and use the configured backend URI.

- [ ] **Step 4: Implement stream lifecycle and PCM frame packing.**

Accumulate 320 `int16_t` samples, then pack `<BBBBHHII` with magic `0xA5`, version `1`, codec `0`, flags, sequence, payload length `640`, stream ID, and sample offset. Send the 656-byte binary message. On stop, zero-pad one partial frame and send `stop` JSON.

- [ ] **Step 5: Run focused host tests.**

Run: `python tests\test_firmware_settings.py`

Expected: PASS.

### Task 4: Connect the existing button/I2S application to the backend client

**Files:**
- Modify: `main/main.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Replace USB recorder start/stop with backend stream start/stop.**

Keep the button debounce and LEDs. Generate a monotonically increasing stream ID and refuse to start if the backend is not ready.

- [ ] **Step 2: Feed converted PCM samples to the backend client.**

Keep the explicit left I2S slot and `>> 16` conversion. Pass every converted sample to the 320-sample accumulator.

- [ ] **Step 3: Build with the ESP-IDF environment.**

Run Ninja with the configured ESP-IDF, tools, Xtensa compiler, and `-j1` to avoid the known compiler parallelism crash.

Expected: generated `build/EdgeAIKWS.bin` and app partition size check pass.

- [ ] **Step 4: Run all host tests and the PCM conversion test.**

Run: `python tests\test_firmware_settings.py`

Run: `gcc tests\pcm_conversion_test.c main\audio_conversion.c -o tests\build\pcm_conversion_test.exe; .\tests\build\pcm_conversion_test.exe`

Expected: both commands pass.

### Task 5: Document hardware/network verification

**Files:**
- Modify: `docs/superpowers/specs/2026-09-26-esp32-websocket-backend-design.md`

- [ ] **Step 1: Record the flash and runtime commands.**

Flash with `idf.py -p COM41 build flash`, run the backend on `192.168.1.42:8765`, and press the button once to start and again to stop.

- [ ] **Step 2: Verify the backend output.**

Confirm `received/stream-<id>.wav` is created with 16 kHz mono PCM and the server log reports frames without gaps or malformed-frame warnings.
