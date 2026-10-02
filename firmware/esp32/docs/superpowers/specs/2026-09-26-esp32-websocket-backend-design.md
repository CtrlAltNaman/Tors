# ESP32 WebSocket Backend Streaming Design

## Goal

Send button-triggered ESP32-S3 microphone audio directly to the existing Python WebSocket backend at `ws://192.168.1.42:8765`, while preserving the current I2S microphone capture and left-slot configuration.

## Scope

- Connect the ESP32-S3 to Wi-Fi as a station.
- Connect to the backend using the official ESP-IDF WebSocket client component.
- Send the backend's frozen v1.0 lifecycle: `hello`, `start`, 640-byte PCM binary frames, and `stop`.
- Buffer I2S samples into 320-sample / 20 ms frames.
- Zero-pad a final partial frame before stopping.
- Keep USB serial recording support out of this change; the WebSocket path becomes the network transport.
- Leave MAX98357A playback disconnected and unimplemented for now.

## Data flow

```text
I2S microphone
  -> 32-bit samples, left slot
  -> signed 16-bit PCM
  -> 320-sample frame accumulator
  -> 16-byte protocol header + 640-byte payload
  -> Wi-Fi WebSocket
  -> backend VoiceServer
```

The backend receives:

```json
{"type":"hello","proto":1,"device_id":"edgeai-esp32s3","codecs":["pcm_s16le"],"sample_rate":16000}
```

For each button press, the device sends a `start` message with `stream_id`, `pcm_s16le`, 16000 Hz, mono, and 20 ms frame metadata. Each binary frame uses the backend's 16-byte little-endian header, codec `0`, 640-byte payload, sequential frame number, and 320-sample offset. A second button press sends a padded final frame when needed, then `stop`.

## Configuration

Wi-Fi SSID, Wi-Fi password, and backend URI are supplied through a local firmware configuration header. The password is not duplicated in documentation or tests.

## Failure handling

- Wi-Fi and WebSocket connection failures are logged on UART0 and retried.
- A button press while the backend is unavailable does not start a local network stream.
- Send failures stop the active stream and return to idle.
- The existing red/blue LED state remains the local recording indicator.

## Testing

- Host tests validate the WebSocket dependency declaration, backend URI, lifecycle strings, frame constants, and 320-sample buffering configuration.
- Existing PCM conversion tests remain green.
- ESP-IDF build must complete and produce a flashable binary.
- Hardware verification requires the ESP32 and server to be able to reach each other at `192.168.1.42:8765`.
