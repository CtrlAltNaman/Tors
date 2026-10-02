# Local Diagnostics Implementation Plan

> Execute inline with the executing-plans skill, per the user's approval to implement the proposed local log page.

**Goal:** Show RAM and microphone packet accounting in compact serial summaries and at the device's local HTTP address.

**Architecture:** Capture only increments existing frame counters. The main task publishes a five-second snapshot; a separate, low-priority HTTP task serves a flash-resident page and bounded recent application events. No global SDK log interception, unbounded history, audio payloads, or credentials are exposed.

**Tech Stack:** ESP-IDF 5.5.2, FreeRTOS, esp_http_server, native C tests.

## Implementation sequence

- [x] Add `tests/diagnostic_store_test.c` exercising empty history, order, wrap, long-line termination, overwritten cursors, snapshot replacement, and actual elapsed-time frame rates. Run with native GCC and confirm missing implementation fails.
- [x] Add `main/diagnostic_store.h` and `.c`: 12 bounded event entries, a bounded latest snapshot, frame-rate helper; caller owns synchronization. Run the test to green.
- [x] Add `main/device_diagnostics.h` and `.c`: short-lock copies only, explicit application event logging, snapshot publication, GET `/` and `/logs`; HTML refreshes every five seconds using plain text and `textContent`. Limit HTTP sockets, stack, priority and timeouts; handle startup/registration failures without stopping capture.
- [x] Modify `main/main.c`: five-second RAM/MIC/KWS/health summaries, cumulative/delta microphone frames, PCM bytes, measured rate, sent WebSocket frames, and drop counters. Static `.data + .bss` reported separately from internal heap; arena is a subset of static RAM. Publish same snapshot locally and add compact packet/RAM metrics to existing backend telemetry.
- [x] Modify `main/backend_client.c`: start local diagnostics after IP acquisition; retain connection/error/playback events in bounded history. Print current HTTP URL. Add `esp_http_server` and new sources to `main/CMakeLists.txt`.
- [x] Run existing host tests and full ESP-IDF build; inspect memory growth. Copy verified application binary to the existing `bin/EdgeAIKWS_HelloTors_WebSocket.bin` and verify hashes.
- [x] Document browser access, frame/heap definitions, five-second cadence, bounded/reboot-cleared history, LAN-only unauthenticated access, and physical-device verification still required. Do not flash automatically or modify `record.py`.

## Verification outcome

Native diagnostic-store and stream-policy tests, embedded-page script tests, and the three existing Python checks pass. ESP-IDF build passes; final application size is `0x122d30` bytes. Packaged and build binary SHA-256 hashes match. No Git repository exists in this workspace, so branch/merge workflow is not applicable. No flash or real-device HTTP/capture validation was performed.

## Acceptance checks

Healthy steady capture is approximately 50 complete PCM frames/s, 320 samples and 640 PCM bytes/frame. Packet counters include captured ambient audio, but upload remains KWS-gated. Local page stays usable without the remote backend, provided Wi-Fi is connected. No HTTP send or formatting runs in the microphone frame loop. RAM usage labels do not claim that free heap equals total physical RAM or that arena usage must be added twice.
