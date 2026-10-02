# Playback Prebuffer and Local Test Implementation Plan

> **For agentic workers:** Use subagent-driven-development for the independent
> local-backend task; the controller implements the firmware and integration.

**Goal:** Buffer five packets before speaker playback and provide a paced,
ASR-free local sender using the user's OGG recording converted to PCM16 WAV.

**Architecture:** Keep receive validation and bounded PSRAM storage in
`playback_buffer.c`; expose readiness without consuming frames. Gate only TX
startup/restart, never pause normal streaming while waiting for five more packets.
The local server runs its one-shot playback sender alongside the existing receive
loop. The controller verifies both sides against the unchanged binary protocol.

**Tech Stack:** ESP-IDF/FreeRTOS C, Python asyncio/websockets 16, FFmpeg,
native GCC tests, Python unittest and loopback WebSockets.

No Git metadata is present; work in place, preserve unrelated files and existing
binaries, and do not flash or stop user-owned processes automatically.

## Task 1: Firmware buffer policy and timing

Files: `main/playback_buffer.c/h`, `tests/playback_buffer_test.c`,
`tests/playback_stubs/esp_timer.h`.

- [x] Add a failing native assertion that one non-final packet is not ready,
  but five packets are ready; ended clips with fewer packets must be ready.
- [x] Add readiness API, increase `PLAYBACK_QUEUE_FRAMES` to 64 and set
  `PLAYBACK_PREBUFFER_FRAMES` to 5. Readiness never removes data and is not a
  per-frame threshold. Preserve generation cancellation and 40 ms backpressure.
- [x] Exercise FIFO at full capacity, overflow, reset, one-packet LAST and end
  without LAST. Test receive-gap and write-duration high-water counters with a
  clock stub; reset the gap baseline between clips.
- [x] Run native GCC with `-std=c11 -Wall -Wextra -Werror -Imain
  -Itests/playback_stubs tests/playback_buffer_test.c main/playback_buffer.c`.

## Task 2: Speaker integration and diagnostics

Files: `main/main.c`, `tests/test_playback_integration.py`,
`tests/test_psram_led_settings.py`, `tests/check_psram_link_layout.py`.

- [x] Add failing integration checks for startup readiness and compact timing
  summaries. Keep zero-fill DMA behavior and completion only after final drain.
- [x] On generation change, disable stale TX before waiting for the new prebuffer.
  Poll readiness with a bounded task delay (not a busy loop). Bypass for completed
  short clips. After a long receive gap, require the startup threshold again.
- [x] Measure I2S-write elapsed time in task context. Report max receive gap and
  max write duration in the existing PLAY line and backend metrics, with queue
  capacity from the constant. Do not add per-packet logging or claim DMA proof.
- [x] Run Python integration/log-size checks and the PCM gain regression test.

## Task 3: Independent local backend sender

Files owned by worker: `local_backend.py`, `local_playback.py` if needed,
`tests/test_local_playback.py`, `docs/LOCAL_PLAYBACK_TEST.md`.

- [x] Write failing unittest cases for CLI modes, strict WAV validation and a
  smooth bounded tone, then implement `--play-wav` and `--test-tone` exclusively.
- [x] Send text play_start, binary frames using `<BBBBHHII>`, and text play_stop.
  Frame PCM in 640-byte blocks with final zero-padding, FIRST/LAST, sequence
  modulo 65536, u32 audio ID and sample offsets stepping by 320.
- [x] Pace using monotonic deadlines: 20 ms intervals, account for send time,
  and do not emit a catch-up burst after a long stall. Tests inject a clock and
  transport delay; loopback test validates the real socket lifecycle.
- [x] Start one sender after valid hello_ack and a startup grace delay, while
  receiving continues. Cancel on disconnect and ignore repeated hello for auto
  playback. Distinguish sent from device-confirmed complete/error.
- [x] Run `python tests/test_local_playback.py` and `python tests/test_local_backend.py`.
  Document the prepared OGG-derived WAV command and local IP configuration.

## Task 4: Asset, integration, review and build

- [x] Inspect `SampleAudio.ogg` with ffprobe. Convert without overwriting:
  `ffmpeg -n -i SampleAudio.ogg -vn -ac 1 -ar 16000 -c:a pcm_s16le
  test_audio/SampleAudio_16k_mono.wav`; verify duration, format and sample count.
- [x] Review worker changes for spec compliance and code quality. Run real
  loopback send/receive on an ephemeral localhost port, not the user's server.
- [x] Run all relevant firmware, local-backend and UI regressions. Build using
  the installed ESP-IDF 5.5.2 environment; verify PSRAM queue is 41,728 bytes.
- [x] Save `bin/EdgeAIKWS_T045_N16R8_Prebuffer5_Volume70.bin` if absent, verify
  hash against build output, and document measured linked memory and limitations.
- [x] Provide flash and local playback commands. Hardware acoustics remain
  unverified until the user flashes and listens; preserve remote backend config.

## Verification result

- ESP-IDF build passed; app binary 1227280 bytes, saved with matching SHA-256.
- Internal data+BSS 163668 bytes; external BSS 96064 bytes; speaker queue 41728 bytes in PSRAM.
- 10 executable speaker-policy/native-buffer/gain tests passed; firmware integration, placement, diagnostics and prior regression checks passed.
- Local playback suite: 30 tests passed; existing local backend tests passed.
- Actual sample loopback: 998 packets, 319256 samples, PCM byte-exact; 19.951 s first-to-last (19.940 s nominal), 9 concurrent metrics acknowledgements.
- Reviews caught and fixes cover terminal queue publication/timeout acknowledgement, Windows high-resolution pacing, nonblocking recording writes and malformed control fields.
- Original OGG preserved. No hardware flash, LAN listener, network configuration change or acoustic verification performed.
- Current checked laptop IP is 192.168.1.2; firmware destination remains 192.168.1.30.
