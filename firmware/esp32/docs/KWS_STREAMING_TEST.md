# KWS-triggered laptop streaming

For RAM/captured-frame summaries and browser access at the ESP32's local IP, see [Local diagnostics](LOCAL_DIAGNOSTICS.md).

## Behavior

- Two startup beeps remain. Keep quiet afterward for two seconds of ambient calibration; wait for `Listening: ambient_rms=...`.
- The configured WebSocket is `ws://192.168.1.3:8765/v1/stream` (see `main/network_config.h`). The server must send `hello_ack` before audio can start.
- A successful Hello Tors detection pulses GPIO 7 (red LED) for 300 ms. Audio is uploaded only after detection, not continuously while listening. Connection control and metrics still run while idle.
- Upload includes 800 ms of buffered audio, followed by live 16 kHz, signed 16-bit mono PCM in 20 ms frames using the existing 16-byte binary header.
- After at least two seconds of live capture, 1.5 seconds continuously below the speech-energy threshold ends the stream with `stop`, reason `silence`. There is no fixed five-second cutoff.
- KWS pauses during a session, speaker playback, local-save completion and flash maintenance. Its feature window is cleared before listening resumes.
- A disconnected backend drops the upload, but local storage can still save a KWS-triggered clip (three retained, max 30 seconds each). Old sessions are not uploaded on reconnect. See [local recordings](LED_STATES_AND_RECORDINGS.md).
- The button remains USB recording only; `record.py` is unchanged.

## Capture and buffering

Microphone capture runs on core 0 at priority 8; inference runs on core 1 at priority 2. Capture does not invoke the model or send network/USB audio. A 16-frame (320 ms) queue feeds inference; a 60-frame (1.2 second) PCM history feeds streaming. The 800 ms prebuffer leaves 400 ms of initial network headroom.

Queue gaps reset the KWS feature window. If the network falls behind far enough to overwrite unsent history, the session stops with `buffer_overrun` rather than silently sending mismatched data. Buffers are bounded: this does not guarantee lossless delivery through an arbitrarily slow/disconnected network.

## Endpoint tuning

The threshold is `max(128, calibrated_noise_rms * 3)` in PCM amplitude units, frozen per session. Idle background estimation adapts slowly to modest noise changes. Parameters are in `main/stream_policy.h`.

This is an energy-based endpoint, not a trained speech/noise classifier: loud machinery can keep a stream open, soft speech can stop it early, and speaking during calibration can bias the threshold. Tune only after checking actual `rms` and `ambient` logs.

KWS output is directly dequantized because the model already contains Softmax. The current user-confirmed threshold is 0.45 (package default: 0.50), with two consecutive positive evaluations and a one-second cooldown. See [exact model integration](EXACT_MODEL_INTEGRATION.md) for frontend parity, boot self-tests, memory figures and remaining hardware validation.

## Flash and test

From an ESP-IDF-enabled terminal in the project:

```powershell
idf.py -p COM21 flash monitor
```

1. Keep the laptop backend running and confirm `Backend hello acknowledged` on the ESP32.
2. Wait for ambient calibration, say **Hello Tors**, then speak normally. Check the red pulse and `KWS stream ... started`.
3. Speak longer than five seconds: streaming should continue. A one-second pause should not end it.
4. Stop speaking for 1.5 seconds: expect `Session stopped: silence`, a saved recording on the test backend, and a local WAV. Wait for storage readiness before repeating with a fresh wake phrase.
5. Check five-second diagnostics: `dma_overflows`, `kws_queue_drops`, `stream_overflows`, and `read_errors` should remain zero after startup. Increasing counters mean capture, inference throughput, or network delivery needs attention. Check `heap` and task stack watermarks as well.
6. Leave the device listening without the phrase: there should be no audio uploads unless the model falsely triggers. This change does not establish a measured false-activation rate.

The updated application binary is `bin/EdgeAIKWS_T045_LED_LocalRecordings.bin` (application only, flash offset `0x10000`). Earlier binaries are not updated. Prefer `idf.py flash` to install the matching bootloader and partition table too.

Host tests cover quantized score decoding, DC removal, full-scale RMS, ambient calibration, short pauses, long speech, quiet endpointing, and history wraparound. Build success and these tests do not establish real-device timing, audio quality, or the competition's RAM/CPU limits.
