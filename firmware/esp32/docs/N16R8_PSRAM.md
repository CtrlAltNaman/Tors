# N16R8 PSRAM and corrected LED mapping

For the newer speaker buffering build, see [Speaker playback fix](SPEAKER_PLAYBACK_FIX.md).
The resource figures below describe the original PSRAM-enablement build.

Scope approved by the user on 2026-09-27: swap physical LED colors and use the
confirmed N16R8 board's PSRAM. No calibration/parity mode, gain, DC filtering,
energy gating, model, threshold, CPU-frequency or inference-cadence changes.

## Configuration and placement

- Red: GPIO15. Green: GPIO7. Existing mutually exclusive state patterns remain.
- 16 MB flash, octal PSRAM at 80 MHz, automatic chip-size detection.
- PSRAM boot initialization and memory test enabled. Failure aborts startup;
  there is no silent non-PSRAM fallback because external BSS is required.
- Audio history: 38408 bytes external BSS (38400 PCM bytes plus frame count).
- Upload staging: 640-byte PCM frame and 656-byte protocol frame external BSS.
- ESP-IDF's external-BSS placement also moves eligible networking-library data.
- Wi-Fi/lwIP prefer PSRAM for supported allocations. Not every network buffer
  can move; DMA requirements still apply. Normal malloc retains the 16384-byte
  internal-preference threshold and a 32768-byte internal-only reserve.
- Arena remains 114688 bytes in internal BSS. I2S staging, DMA, interrupt state
  and task stacks remain internal. Code/model weights remain in flash.

Only task-context buffers are explicitly moved. The I2S overflow ISR does not
access external audio history. Local flash storage stages audio into its
internal scratch buffer before writing; flash operations still have timing
costs and can cause gaps, which the existing continuity checks detect.

Placement uses ESP-IDF's supported `EXT_RAM_BSS_ATTR` mechanism:
[ESP-IDF v5.5 external RAM guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/external-ram.html).
This is a memory-placement change, not a reduction in total application RAM.
It does not establish compliance with the competition's <256 KB total limit.

## Logs and validation

Verified build: app binary **1224064 bytes** (`0x12ad80`). Linked internal
data+BSS is **163100 bytes**, down **51208 bytes** from the previous 214308-byte
build. External BSS is **53680 bytes**, including eligible library data.
These are static allocations, not runtime free heap or total RAM consumption.
The audio history and both upload buffers were checked in the linked ELF;
the arena, I2S buffers, epoch tracking and ISR lock were checked as internal.

App-only artifact: `bin/EdgeAIKWS_T045_N16R8_PSRAM.bin`, offset `0x10000`.
Earlier artifacts remain unchanged. Prefer full-project flashing below.

Every five seconds, serial and the existing `http://<device-IP>/` logs show:

- `RAM internal_heap ... static_data_bss=... arena=...`: internal only.
- `PSRAM total=... heap_used=... free=... min_free=... largest=... static_bss=...`.

External static BSS is excluded from external heap usage; do not double-count
the arena, which is already included in internal static BSS. Backend metrics
carry the same PSRAM measurements as separate `psram_*_bytes` fields.
`minimum_free_heap_bytes` stays internal, matching inference before/after heap.

Build verification checks configuration, unchanged model integration, LED
state mutual exclusion, and linked symbol placement. Hardware is not flashed
automatically. From the configured ESP-IDF terminal:

```powershell
idf.py -p COM21 flash monitor
```

Use the full project flash so the rebuilt bootloader and application match.
On the board, check PSRAM detection/memory test, both KWS self-tests, the new
color mapping, 50 fps capture, overflow counters, and heap during Wi-Fi,
streaming, speaker playback and local recording downloads. Actual free heap
and PSRAM speed/stability require this hardware run.
