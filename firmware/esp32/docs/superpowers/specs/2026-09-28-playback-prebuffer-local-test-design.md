# Playback prebuffer and ASR-free speaker testing

## Scope

Improve the existing ESP32-S3 speaker path and add playback to `local_backend.py`.
Keep the microphone, KWS model/threshold, Wi-Fi credentials, backend address,
16 kHz mono PCM16 wire protocol, and 70% speaker gain unchanged. Do not flash
the board or replace a running backend automatically.

## Firmware design

- Wait for five complete packets (100 ms of audio) before starting a new clip
  or restarting after a long receive gap. A finished shorter clip bypasses the
  threshold, so a single-packet clip cannot become stuck waiting for five.
  If an incomplete slow sender fails to fill the prebuffer within 250 ms, warn
  once for that startup and play what is available rather than waiting forever.
- Expand the bounded software queue from 16 to 64 frames in PSRAM: 41,728 bytes
  versus 10,432 bytes, an increase of 31,296 bytes. DMA memory remains internal.
  Capacity is 1.28 seconds; the startup threshold remains five, not 64.
- Keep the existing bounded receive backpressure, strict frame validation,
  generation-based cancellation and final DMA drain. Never replay the previous
  PCM packet to conceal an empty queue. Persistent overload remains an error.
- Keep network sends out of the speaker task. The existing completion message
  continues to report `complete` or `playback_error` after the tail drains.
- Add compact packet-arrival-gap and I2S-write-duration maxima to periodic
  diagnostics. These are software timing observations, not acoustic measurements.
  The existing `underruns` counter still reports long receive gaps only.
- Prebuffering adds a deliberate startup wait. Existing DMA startup/drain delays
  are additional; 100 ms is buffered audio duration, not a promise of total latency.

## Local test backend

- `python local_backend.py --test-tone` sends a bounded, smooth generated tone.
- `python local_backend.py --play-wav path.wav` sends one validated WAV clip.
  Require uncompressed 16 kHz, mono, signed PCM16 WAV and reject incompatible
  files clearly. Do not silently relabel sample rates or add codec dependencies.
- User also requested the repository's `SampleAudio.ogg`: convert it with the
  installed FFmpeg to `test_audio/SampleAudio_16k_mono.wav`, preserve the original,
  and use `--play-wav` to send the decoded samples (not OGG bytes) on the wire.
- Playback is optional; without either flag, microphone recording works as before.
- Send once per connection after valid `hello`/`hello_ack`, with a short startup
  grace period. Maintain the receive loop concurrently to handle microphone
  frames, metrics, disconnects and the device's playback-completion message.
- Use one `play_start`, 656-byte binary messages paced at 20 ms, and `play_stop`.
  FIRST/LAST flags, sequence wrap, sample offsets and final zero-padding match
  firmware. Do not burst missed deadlines to catch up after a long scheduling stall.
- Cancel the sender on disconnect, reject duplicate automatic playback on a
  repeated hello within one connection, and log send completion separately from
  device playback completion. ASR is neither loaded nor required.
- To use this laptop, the device must point at its reachable LAN IP and this
  server's port/path. A replacement process on another laptop cannot take over
  the configured remote IP automatically. Document this; preserve configuration.

## Validation

- Native C tests: startup threshold, short/final clips, reset cancellation, FIFO,
  bounded bursts/backpressure, malformed packets, timing counters and rebuffering.
- Python tests: WAV validation, waveform generation, header/padding correctness,
  pacing (including delayed sends), hello lifecycle, cancellation and completion.
- Run a loopback WebSocket test with a simulated device; no hardware audio is
  emitted during automated tests. Run existing mic/KWS/LED/recording regressions.
- Build ESP-IDF and inspect linked PSRAM placement. Save a distinct app binary
  without overwriting previous artifacts. Real audio quality still needs flashing
  and a speaker listening test; do not describe host checks as hardware proof.

## Alternatives

Immediate playback has the least startup delay but no explicit prebuffer.
Full-clip buffering avoids delivery gaps after playback starts but requires more
RAM and waits for the entire response. The five-packet bounded design is the
proposed middle ground, with a reproducible local sender to isolate server pacing.

## Review

User approved this design and requested implementation, including the OGG sample.
No Git repository is available in this workspace; no commit can be made.
