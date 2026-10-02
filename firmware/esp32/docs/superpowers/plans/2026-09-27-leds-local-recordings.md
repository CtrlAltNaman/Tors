# LED states and local recordings implementation plan

**Goal:** Implement the user-approved mutually exclusive red/green LED patterns and
three persistent, downloadable KWS-triggered recordings, capped at 30 seconds each.

**Architecture:** Pure, host-tested LED policy drives a single timer-owned GPIO
writer. A low-priority recorder reads the existing synchronized PCM history with
its own cursor, independently of WebSocket delivery. Four bounded 1 MiB raw slots
in the existing storage partition hold three committed recordings and one spare.
Only recognized recorder slots or verified-erased space may be used. No automatic
filesystem format or partition-table change. WAV headers are generated for HTTP.

**Tech stack:** ESP-IDF 5.5.2, FreeRTOS, esp_partition, esp_timer, esp_http_server,
portable C policy/format tests, existing Python/Node regression tests.

User approved the preceding LED/recording design with “this works”; execute inline.
GPIO15 is assumed to be the external green LED; no claim to control power LEDs.
No Git repository is available, so preserve originals in a new saved directory.

## Tasks

- [x] Test and implement `main/device_state.c/h`: state priority, mutually exclusive
  patterns, names. Test every state at every millisecond across pattern periods.
- [x] Test and implement `main/recording_format.c/h`: 16 kHz mono PCM16 WAV header,
  durable metadata and CRC, strict numeric download paths and byte ranges.
- [x] Implement `main/device_leds.c/h`: one timer callback writes GPIO7/15, switches
  old color off before new color on, fatal error latches, boot green blink.
- [x] Implement `main/local_recordings.c/h`: flash ownership/blank checks, committed
  metadata, monotonic IDs, retention, reader pinning, frame cap, partial-file flags,
  task-owned 1920-byte scratch. End on history loss rather than stitch across a gap.
  Prepare erased spare while no recording is active; expose PREPARING state and
  pause KWS through maintenance. Never erase a pinned download or unknown data.
- [x] Implement `main/recordings_http.c/h`: `/recordings` list, play/download URLs,
  immutable numeric IDs, Range handling, bounded responses and 416/404 errors.
  Storage-unavailable status is shown on the listing page rather than hiding it.
- [x] Integrate main: offline KWS starts local capture; network error disables only
  upload, not local capture; shared ambient endpoint ends both. Local 30-second
  cap does not stop upload. LED status and logs use the same enum. Preserve model,
  threshold, pins, speaker, recording script and USB mini-project.
- [x] Run portable tests plus existing frontend, firmware, backend and page tests;
  build and size report. Review error/race/retention paths. Package a distinctly
  named binary, document URLs, LED patterns, retention and hardware test steps.

## Verification commands

```powershell
gcc -std=c11 -Wall -Wextra -Werror -Imain tests/device_state_test.c main/device_state.c -o tests/device_state_test.exe
./tests/device_state_test.exe
gcc -std=c11 -Wall -Wextra -Werror -Imain tests/recording_format_test.c main/recording_format.c -o tests/recording_format_test.exe
./tests/recording_format_test.exe
python tests/test_kws_firmware_integration.py
python tests/test_firmware_settings.py
python tests/test_local_backend.py
node tests/test_diagnostics_page.cjs
idf.py build
idf.py size
```

On-board flashing, real recording quality, flash-write interference, heap/stack
headroom, browser audio playback and reboot persistence require hardware tests;
host/build checks alone must not be described as proving these.
