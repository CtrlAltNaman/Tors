# ESP32 local diagnostics

After flashing, look for `Local logs: http://<device-IP>/` on the serial monitor. Open that address on a laptop/phone on the same reachable LAN. Use the **ESP32's** IP, not the laptop/backend IP. DHCP may change the device address; the URL is announced on each successful IP acquisition.

The device serves a read-only HTTP page at `/`, refreshing approximately every five seconds, and plain text at `/logs`. It does not push logs to its own address. The page works without a connected backend as long as Wi-Fi is connected. Existing WebSocket metrics still go to the laptop backend.

The page also links to `/recordings` for persistent microphone WAV playback and
downloads. See [LED states and recordings](LED_STATES_AND_RECORDINGS.md).

## Compact serial and browser summary

Every five seconds, five labeled lines show:

- `STATUS`: uptime, shared LED/device state, backend readiness, local recorder status, saved count, errors and current/last clip PCM bytes. `RECORDING` can mean local-only, network or USB capture; `PREPARING` pauses KWS during flash maintenance.
- `RAM`: internal 8-bit heap allocated/free bytes, minimum free bytes, largest allocatable block, linked `.data + .bss`, and arena used/reserved bytes. The arena is already included in static RAM; do not add it again. Heap allocation includes task stacks and dynamically allocated queues/buffers. These are not a complete total-physical-RAM accounting: IRAM code, reserved regions, allocator overhead, and other memory capabilities are not included. The minimum-free value is ESP-IDF's aggregate per-region low-water statistic.
- `MIC`: cumulative complete captured PCM frames, frames added during the actual reporting interval, measured frames/second, cumulative PCM bytes, cumulative successful WebSocket audio frame writes and interval delta, current RMS and estimated ambient RMS.
- `KWS`: latest score, peak score over this report interval, peak 100-ms RMS in dBFS, evaluation stride, boot self-test result, hit/inference counts, average inference time/cycles and sampled internal 8-bit free heap before/after inference. Peak levels cover samples consumed by KWS, not speaker playback or upload periods; peaks reset after each report.
- `HEALTH`: DMA/queue/stream overflow counts, capture errors, uploads skipped for an unavailable backend (local recording can still succeed), and capture/KWS/recorder minimum stack headroom in bytes.

One microphone frame is **20 ms, 320 samples, 640 PCM bytes**. Healthy steady capture is approximately **50 frames/s** and **32,000 PCM bytes/s**. These are application PCM frames, not individual I2S driver calls or physical Ethernet/Wi-Fi packets. Initial settling frames are intentionally excluded, so the first interval can be below 50 fps. Counts include ambient audio captured while idle; audio upload remains KWS-triggered.

`WS_sent` counts complete frames accepted by successful WebSocket writes, including prebuffer audio. It does **not** prove server receipt or storage. Wire messages add the existing 16-byte audio header plus WebSocket/TCP/Wi-Fi overhead. Drops stay cumulative after the startup settling period; zeros are the desired result.

## Recent events and resource limits

The page retains the latest summary and last 12 selected application events (connection changes, KWS hits, stream start/stop, backend errors and playback events). It does not mirror every ESP-IDF/ROM boot log. Entries are bounded to 159 characters, and all history clears on reboot. Full SDK diagnostics remain on serial.

History and formatting use about 4.8 KB of additional static memory; the HTTP task reserves 4 KB of heap for its stack, plus bounded server/socket/request allocations. Only two simultaneous HTTP clients are allowed, with one-second socket timeouts. The server task runs at priority 1. No per-frame formatting or HTTP writes occur in the capture task. HTTP startup failures are logged and do not halt microphone capture.

Access is **unauthenticated, unencrypted HTTP for a trusted LAN**. Retained voice recordings are now downloadable through `/recordings`; anyone able to reach the device can access them. No control endpoints or Wi-Fi passwords are exposed. Do not port-forward it to the public Internet.

## Verification on hardware

```powershell
idf.py -p COM21 flash monitor
```

1. Confirm the printed local URL opens and shows RAM/MIC summaries.
2. Check approximately 50 fps after settling, and increasing `MIC frames` even while `WS_sent` stays fixed before KWS.
3. Trigger Hello Tors; confirm red pulse, RECORDING state, and increasing `WS_sent` when the backend is ready. Return to ambient noise, confirm a `silence` stop event, and check the local WAV list.
4. Keep the page open while speaking; overflow counters should remain zero and heap/stack headroom should remain stable.
5. Stop the laptop backend: the local page should still refresh, while backend state becomes OFFLINE. A lost Wi-Fi connection makes the page unreachable until reconnection.

Native C tests verify bounded storage, wraparound, truncation and frame-rate calculations. A Node test executes the embedded page script for refresh/error handling. Full ESP-IDF compilation verifies the firmware integration; these host checks do not replace real-device heap, timing and browser-access tests.
