# LED states and local microphone recordings

## Flash and open

From the main project in an ESP-IDF terminal:

```powershell
idf.py -p COM21 flash monitor
```

Open the ESP32 address printed by `Local logs: http://<device-IP>/`.
Click **Play / download microphone recordings**, or visit:

```text
http://<device-IP>/recordings
```

Use the ESP32's DHCP address, not the backend laptop's address. The ESP32 must
be reachable over Wi-Fi, but the backend does not need to be running. There is
no new access-point mode. Refresh the recording list after a clip completes;
the list deliberately does not auto-refresh and interrupt browser playback.

Current N16R8 app-only binary: `bin/EdgeAIKWS_T045_N16R8_Prebuffer5_Volume70.bin`, flash offset
`0x10000`. Prefer `idf.py flash` to install the matching partition table and
bootloader. Earlier binaries remain unchanged and do not contain this feature.

## LED state definitions

Speaker receive-buffer and tail-drain changes are documented in
[Speaker playback fix](SPEAKER_PLAYBACK_FIX.md).

External active-high LEDs: **red GPIO15**, **green GPIO7**, corrected to the
user-confirmed physical colors on 2026-09-27. This does not control an
onboard power LED. One timer callback owns both outputs, switching the previous
color off before switching the next color on. USB stop no longer latches GPIO15.

| State | Pattern |
| --- | --- |
| BOOT / CALIBRATING | Green 500 ms on / 500 ms off |
| PREPARING | Same slow green blink, while preparing storage |
| LISTENING | Green steady; backend ready |
| OFFLINE | Two 100-ms green flashes every two seconds; backend not ready |
| TRIGGERED | Red steady for the approximately 300-ms KWS indication |
| RECORDING | Red 250 ms on / 250 ms off; local capture, upload session or USB recording |
| PLAYBACK | Green 100 ms on / 100 ms off |
| ERROR | Red 100 ms on / 100 ms off; latched fatal firmware error |

Priority: fatal error > trigger indication > recording > playback > preparing >
calibration > listening/offline. States never request both LEDs simultaneously.
The serial/local-page `STATUS state=` uses the same enum as the LED controller.
Storage-only failures are logged in `local=...` and do not halt working KWS/network
functions or latch the fatal LED state.

## What is saved

- Threshold remains **0.45**, two consecutive positive evaluations, one-second
  cooldown. Model, features and microphone pins are unchanged.
- A KWS detection starts a local recording when storage is ready, including the
  existing 800-ms PCM prebuffer. This can include part of the wake phrase.
- The saved audio is the actual captured PCM16: mono, 16000 Hz, without gain,
  denoising, injected noise or additional waveform processing.
- Local clips end at the existing ambient-noise endpoint, or at **30 seconds
  total including prebuffer**, whichever occurs first.
- Online WebSocket upload has its own cursor. The local cap does not stop it.
  A network failure stops upload but lets the local clip continue to its endpoint.
- With no backend, a local-only session ends on ambient noise or when local
  recording completes/fails. It is not uploaded later on reconnection.
- USB button recording remains independent; these saved clips are KWS-triggered,
  not continuous recordings of everything heard by the mic.

The page shows the latest **three completed recordings**, duration, stream ID,
completion reason and PCM CRC32. Each has a browser audio player and download link.
Completed clips survive normal power cycles. A recording interrupted before its
metadata commit is not exposed as a valid WAV.

`silence` is a normal endpoint. `30s_limit` is the local cap. `capture_gap` means
the contiguous recording ended early because history was overwritten, capture
errors/DMA-loss counters changed, or capture stopped making progress. Missing
audio is not silently concatenated or replaced with invented silence. A clip
with zero contiguous frames is discarded and logged rather than offered as a WAV.

The energy endpoint is not a trained VAD: loud noise can prolong a recording and
soft speech can end it early. It uses `max(128, ambient_rms * 3)`, at least two
seconds of live capture and 1.5 seconds of consecutive quiet frames.

## Storage and safety

The existing `storage` partition is at `0xA10000`, length `0x5F0000`. Its first
four 1-MiB slots are used for PCM plus ownership/commit metadata; the rest is
untouched. The partition-table subtype remains SPIFFS, but **this recorder uses
raw slots, not a mounted SPIFFS filesystem**. Do not mount/format that partition
as SPIFFS while retaining these recordings.

Three slots hold retained clips and the fourth provides an erased spare. HTTP
generates a standard 44-byte WAV header followed by that slot's PCM. A fourth
completed recording expires the oldest from the list; its slot is recycled only
after existing downloads release it. Normal retention deletion is permanent;
download any clip you want to keep before more detections occur.

Flash access is partition-relative and bounded. Initialization accepts only
recognized recorder-owned slots or slots verified completely erased. Unknown
contents disable local storage with `unrecognized_storage_preserved`; they are
not automatically formatted or erased. KWS and backend can still operate.
If this happens, preserve/back up the old storage before deciding to erase it.

Ownership and a CRC-protected commit record separate incomplete writes from
completed recordings. Payload CRC is checked when loading saved clips at boot.
Cleanup invalidates the commit before erasing data, retaining ownership until
the final header-sector erase, so interrupted recycling can be recognized.

After a session, preparing the next spare may take several seconds. KWS pauses
during this maintenance and resumes with a fresh feature window; the green LED
shows PREPARING. Capture continues, with flash sectors erased individually and
task yields between them. Flash operations can still interfere with real-time
capture: verify HEALTH counters on hardware. There is no claim of zero loss or
unchanged wake latency during storage maintenance.

Downloads pin their slot. If an expired recording is still being downloaded,
storage can report `waiting_for_download`; another local recording may be
unavailable until it finishes. Network uploads remain independently usable.

The page is **unauthenticated, unencrypted, trusted-LAN only** and now exposes
recorded voice. Anyone who can reach this HTTP server can download retained
clips. Do not port-forward it or treat it as private on an untrusted network.

## Resource cost and evidence

Historical pre-PSRAM build: internal static data+BSS **214308 B**, previously
211964 B; increase **2344 B**. The N16R8 PSRAM build is documented in
[N16R8 PSRAM configuration](N16R8_PSRAM.md); these old figures are not its headroom.
The recorder also allocates a **4096-byte task stack** plus RTOS task overhead,
and LED timer/HTTP requests have additional small dynamic costs. No 30-second
RAM audio buffer and no SPIFFS heap/cache are allocated. Actual live free heap
and stack margins must be checked after Wi-Fi connects and during downloads.

Pre-PSRAM application binary: **1211056 bytes** (0x127AB0), 88% of the 10-MiB app partition
remains free. The reserved tensor arena stays 114688 B and is included in BSS.
The full application is still not demonstrated to meet the competition RAM/CPU
limits. Recording/flash activity adds costs outside the model.

Host tests exercise LED state priority/mutual exclusion, WAV/CRC/path/range
formats, the actual storage code with a simulated NOR-flash adapter, and the
actual HTTP handlers with simulated requests. They cover retention, downloads
holding old slots, 30-second capping, gaps, power-cycle recovery, write failure,
interrupted cleanup and preservation of unknown flash. They do not reproduce
ESP32 flash timing or prove microphone audio quality.

## Hardware acceptance checks

1. Require both KWS boot self-tests to pass. Wait for calibration and storage
   readiness. Verify only the expected external LED is lit.
2. With the backend stopped, say Hello Tors, speak, then stop. Confirm red trigger
   then red blinking, `local=recording`, and a completed file at `/recordings`.
3. Play/download the WAV and verify intelligible, correctly paced audio and
   expected 16-kHz/16-bit/mono format. Compare its duration with what you said.
4. Repeat with the backend connected; local and backend PCM should correspond
   up to the local cap unless either path reports a gap/failure.
5. Speak past 30 seconds. Local WAV must cap at 30 seconds; online upload must
   continue until the normal endpoint. Check the `30s_limit` label.
6. Reboot after saving; clips should remain. Record a fourth clip; only the
   latest three should be listed. Download anything important beforehand.
7. Check approximately 50 MIC fps, DMA/queue/stream/read counters, local errors,
   heap/largest block and stack margins while recording/downloading/recycling.

This change was built and host-tested, **not flashed or hardware-validated**.
