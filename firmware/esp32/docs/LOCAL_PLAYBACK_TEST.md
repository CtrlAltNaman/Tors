# Local speaker test without ASR

Run these commands from the repository root. The local test backend lives in
the repository-level `local_backend/` folder.
The backend needs Python and the existing `websockets` dependency (tested with
Python 3.12 and websockets 16). If missing, install it with
`python -m pip install websockets`. Playback uses the standard library; it needs
no ASR, model downloads, audio device on the laptop, or runtime codec packages.

Use the prepared WAV decoded from the repository's `SampleAudio.ogg`:

```powershell
python local_backend/local_backend.py --play-wav local_backend/test_audio/SampleAudio_16k_mono.wav
```

The prepared file contains 319,256 samples of uncompressed PCM16, mono, 16000 Hz:
19.9535 seconds and 998 playback packets. Its final packet contains 216 samples
and 104 zero samples of padding. The original Opus recording is 48 kHz mono,
19.9665 seconds including codec padding, so its container duration differs.
The original `SampleAudio.ogg` is preserved. FFmpeg was used separately to prepare
the WAV; the backend never decodes or sends OGG and never sends WAV headers.

For a two-second 440 Hz tone, with 12% peak amplitude and 50 ms fades:

```powershell
python local_backend/local_backend.py --test-tone
```

The two playback flags are mutually exclusive. With neither flag,
`python local_backend/local_backend.py` remains capture-only. Uploaded microphone streams still
save under `local_received` (or `--output-dir PATH`), and metrics are acknowledged
while playback runs. Completed streams are detached from receive state and saved
with `asyncio.to_thread`, so disk writes do not block playback or metrics. Writes
remain serialized, failures are logged, and connection cleanup waits for all
pending saves, including any open stream finalized on disconnect.

## Reach the laptop

The listener defaults to `0.0.0.0:8765`. This binds the laptop's interfaces;
`0.0.0.0` is not a device destination. The firmware must point to this laptop's
reachable LAN IPv4 address, port 8765, and path `/v1/stream`. `localhost` and
`127.0.0.1` on the ESP32 refer to the ESP32 itself, not this laptop.

The firmware is now configured for SSID `Nullpointer-2.4G` (case-sensitive)
and `BACKEND_URI` is `ws://192.168.1.2:8765/v1/stream`, matching the laptop's
latest checked Wi-Fi IPv4. This uses the shared Wi-Fi network, not the laptop
hotspot. The password is configured in `main/network_config.h` and is not
repeated here. Rebuild and flash for these settings to take effect:

```powershell
idf.py -p COM21 build flash monitor
```

Built app-only image for these settings:
`bin/EdgeAIKWS_T045_N16R8_LocalSample.bin` (flash offset `0x10000`).
Full-project flashing is preferred so bootloader and partitions match. Older
binaries are preserved and do not pick up changes to `network_config.h`.

Check `ipconfig` after network changes; if DHCP changes the laptop address,
update `BACKEND_URI` and rebuild. Ensure the ESP32 can reach that network and
allow inbound TCP 8765 through the laptop firewall. Start the sample sender in
a separate terminal with the command above. No server process or firewall rule
is started/changed merely by editing the firmware configuration.

## What to expect

After a valid `hello`, the server sends `hello_ack`, waits approximately one
second, and starts one playback task on that same WebSocket. Repeated hellos on
the connection do not replay the clip; a new connection gets one new attempt.
Disconnecting cancels and awaits the sender, including during its startup grace.

Each attempt sends a text `play_start` with a u32 `audio_id`, `codec=pcm_s16le`,
`sample_rate=16000`, `channels=1`, and `frame_ms=20`. It then sends individual
656-byte binary messages: a 16-byte little-endian `<BBBBHHII>` header and 640
PCM bytes. Flags mark FIRST (1) and LAST (2); a single packet has both (3).
Sequence wraps at 65536; sample offsets advance by 320. The final payload is
zero-padded, followed by a text `play_stop` for the same audio ID.

High-resolution monotonic `time.perf_counter` deadlines include time spent
sending; the frame interval is 20 ms. This avoids the coarser `time.monotonic`
clock on Windows Python 3.12. After a long send or
scheduler stall, the next deadline moves forward; missed frames are not sent in
a catch-up burst. Long host stalls can still exhaust queued audio and cause
underruns, as well as lengthen the test. The firmware's
five-packet prebuffer and 64-frame PSRAM queue are configured separately.

`playback sent audio_id=...` means transmission finished. It does not prove
speaker playback. `device play_stop audio_id=... reason=complete` records the
device's completion report; `reason=playback_error` is logged as a warning.
Neither response triggers `UNKNOWN_CONTROL`. Sender exceptions appear immediately
as `playback send failed` with a traceback. Audio quality still requires an
observed hardware listening test; host and loopback tests cannot establish it.

WAV input is validated before listening: conventional RIFF PCM format tag 1,
16-bit mono at 16000 Hz, nonempty complete samples, valid chunk lengths, at most
120 seconds, and at most 4,888,576 bytes including metadata. Stereo, other sample
rates, compressed/float audio, empty/truncated files, and oversized input fail
with a CLI error. The sender does not resample or silently relabel inputs.

## Host-only checks

```powershell
python local_backend/tests/test_local_playback.py
python local_backend/tests/test_local_backend.py
python local_backend/tests/test_sample_playback.py
```

The playback suite uses deterministic clock/sleep and transport delays for pacing,
plus real WebSocket connections on ephemeral `127.0.0.1` ports for lifecycle,
simultaneous upload/metrics, completion reports, send failures, and cancellation.
It also covers WAV validation, tone shape, packet flags/padding, and sequence wrap.
Regression cases hold disk writes open while playback and metrics continue,
verify cleanup drains saves, and reject malformed codec lists and stream IDs
with protocol errors while keeping the connection usable.
Temporary recordings are cleaned up. It starts no LAN listener, contacts no
ESP32, and produces no audible output. The existing backend regression script
also verifies capture frame parsing and WAV output.

Verified on 2026-09-28: 30 playback tests passed. The full prepared sample
loopback received all 998 packets byte-for-byte, with 9 concurrent metrics
acknowledgements and 19.951 seconds between first and last packet (19.940 seconds
nominal). This is one host-loopback measurement, not a hardware latency guarantee.
