# Paced WebSocket speaker playback

Updated 2026-09-28: five-packet startup prebuffer and ASR-free local sample testing.

Server contract stays 16 kHz, mono PCM16, 320 samples per 20 ms, with the
existing 16-byte binary header. Send `play_start`, paced binary messages,
then `play_stop`. Set FIRST on the first frame, LAST on the final padded
frame, sequence starting at zero, and sample offsets 0, 320, 640, ... .
One active playback clip per device; a new `play_start` replaces queued audio.
The optional local test sender is added separately; microphone/KWS features,
model weights, threshold and remote backend configuration are unchanged.

Speaker output is now set to **70% linear PCM amplitude** using
`SPEAKER_VOLUME_PERCENT` in `main/audio_conversion.h`. The shared output
conversion applies to backend playback and startup beeps only; microphone
capture, KWS input and uploaded audio are unchanged. This is not a claim of
70% perceived loudness. Host tests cover all 65536 PCM16 sample values.

## Firmware changes

- 64-frame / 1280 ms software queue in PSRAM (previously 16 frames / 320 ms).
- At startup/restart, hold the first packet and wait for four more before enabling
  TX: five packets contain 100 ms of audio. LAST or play_stop releases shorter
  clips immediately. The wait is capped at 250 ms for an incomplete stalled sender,
  with a warning before playing available data. Steady playback is not re-gated.
  Existing DMA zero-fill startup latency is additional to this wait.
- Up to 40 ms bounded wait for space during receive bursts. Persistent overload
  still causes counted drops; this is not permission for an unpaced sender.
- Reassembles ESP-IDF receive-event chunks of a 656-byte binary message.
  Checks magic, version, PCM size, ID, sequence and sample offset before playback.
  RFC WebSocket continuation-frame messages are not supported; send each audio
  packet as one binary message/frame, as the backend does with `ws.send(bytes)`.
- New clip / WebSocket reconnect resets the queue and invalidates stale reads.
- TX DMA auto-clear sends silence instead of recycling old samples on underrun.
  See [ESP-IDF I2S documentation](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32s3/api-reference/peripherals/i2s.html).
- The stopped DMA ring is cleared before re-enabling TX. The final write is
  followed by a conservative 180 ms drain before disabling TX (8 x 20 ms DMA
  buffers plus one frame margin). No-data shutdown waits 250 ms, also exceeding
  the DMA ring duration. Startup beeps use the corrected drain time too.
- Speaker completion acknowledgements are queued to the main task: network
  sending cannot block the speaker consumer. Clips with receive/write failures
  report `reason: playback_error`, not `complete`. A superseding clip cancels
  the old queued acknowledgement. This does not measure acoustic playback.
- LAST is published as received only after its queue insertion/failure accounting.
  A receive timeout acknowledges completion only when that generation is ended
  and its software queue is empty, so a just-arrived final packet is not skipped.
- Per-frame warning spam is replaced by five-second `PLAY` counters, included
  in serial, local-IP logs and backend metrics.

Example fields (values depend on the run):

```text
PLAY rx=... submitted=... queue=.../64 peak=... overflow=... invalid=... write_errors=... underruns=... rx_gap_max=...ms write_max=...ms
```

`submitted` counts full PCM frames accepted by I2S, not samples independently
observed at the amplifier. `overflow` is queue-full loss after the bounded wait.
`invalid` covers malformed/out-of-sequence frames. `underruns` counts active-clip
250 ms receive gaps; it is not a count of every short DMA underrun. `HEALTH`
also shows main and speaker stack low-watermarks. Backend's stop message is
logged as audio received, rather than claiming the amplifier has finished.

`rx_gap_max` is the largest observed time between valid complete packets within
a clip (including event-task scheduling/backpressure), excluding inter-clip idle.
`write_max` is the maximum time spent in an I2S write, rounded up to milliseconds.
Both maxima accumulate until reboot. They help isolate timing problems but are
not DMA or acoustic underrun counters. Startup waiting does not count as a write.

## Resources and verification

Linked internal data+BSS: **163668 bytes** (+16 from the 70% volume build).
External BSS: **96064 bytes** (+31296: queue grew from 10432 to 41728 bytes).
The 656-byte assembly buffer remains external. Removed the old four-frame dynamic
queue. Arena remains 114688 bytes internal. These are not runtime heap totals
and do not establish <256 KB overall RAM compliance.

Host tests cover paced delivery, FIFO bursts, queue-full bounded wait, persistent
overflow including dropped LAST, chunk assembly, malformed and out-of-order
frames, and reset invalidation. An executable harness compiles the actual speaker
task with simulated RTOS/I2S adapters: startup, short clips, explicit stop, bounded
fallback, steady playback, gap recovery, cancellation and the timeout/LAST race.
It checks a single acknowledgement after the tail drains. It does not simulate
physical DMA. Other model/frontend/recording/LED tests remain.
ELF verification confirms external placement. Hardware timing and actual sound
still require flashing and a paced backend test; no device was flashed by the agent.

The earlier `main` task stack-overflow report is a separate known issue:
this patch adds stack watermarks but does not resize that stack or claim that
failure is fixed. If it recurs, preserve the log for a targeted stack investigation.

```powershell
python -B tests/speaker_playback_task_test.py -v
python tests/test_sample_playback.py
idf.py build
idf.py -p COM21 flash monitor
```

Verify a short and a long response, two consecutive responses, and reconnect
during playback. Expect no increase in `overflow`, `invalid`, or `write_errors`
with correctly paced packets; confirm mic capture remains near 50 fps. Compare
`rx` / `submitted` deltas after the clip drains. Check main/speaker stack margins.

App-only artifact: `bin/EdgeAIKWS_T045_N16R8_Prebuffer5_Volume70.bin` (offset `0x10000`).
Size: 1227280 bytes. SHA-256:
`728a4d6b9fa7db243c6df0bc5ebd795732345c7911184a684c4668fd44edcd88`.
Previous PlaybackFix and Volume70 binaries are preserved.
Prefer full-project flashing so the matching bootloader/partitions are used.

## Test without ASR

See [Local playback test](LOCAL_PLAYBACK_TEST.md). The repository's
`SampleAudio.ogg` is preserved. `local_backend/test_audio/SampleAudio_16k_mono.wav` is its
FFmpeg-decoded 16 kHz mono PCM16 version: 319256 samples, 19.9535 seconds,
998 wire packets (the final packet is zero-padded). No OGG or WAV header is
sent to the ESP32. Use the local server's optional sender:

```powershell
python local_backend/local_backend.py --play-wav local_backend/test_audio/SampleAudio_16k_mono.wav
```

The ESP32 must connect to this laptop's reachable LAN IP. In the subsequent
local-test configuration, `main/network_config.h` now targets `192.168.1.2` on
`Nullpointer-2.4G`; see [Local playback test](LOCAL_PLAYBACK_TEST.md).
The earlier Prebuffer5 binary above is preserved with its original backend
settings. Use a fresh project build for the updated network configuration.
