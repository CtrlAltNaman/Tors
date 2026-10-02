# EdgeAIKWS SIH Deck — Design Specification

Date: 2026-09-29  
Audience: SIH jury  
Deliverable: editable PowerPoint, eight slides (six core slides plus two technical supplements)

## Purpose and source problem

Present the EdgeAIKWS prototype against SIH Problem Statement 26172, **“Low Latency and Efficient Voice Activator for Edge Devices.”** The problem owner is ISRO / Department of Space, category Hardware, theme Smart Automation. Dhvani Kavach is a visual and narrative reference only; its unrelated adaptive-noise-cancellation content must not appear as EdgeAIKWS content.

The central story is edge-first: an ESP32-S3 continuously listens for the custom phrase “Hello Tors”; only after local detection does it send buffered/live speech to a backend for ASR. State the intended privacy and uplink benefit as an architectural property, not as a measured performance result.

## Slide structure

1. **Title** — EdgeAIKWS; SIH 26172 and exact problem title; ISRO / Department of Space; Hardware; Smart Automation. Do not invent team/member details.
2. **Problem and requirements** — Cloud-only cost/privacy/latency motivation; custom-keyword and open-source framework requirements; evaluation limits (<256 KB RAM, <10% idle CPU), detection quality, false activations, and post-keyword-to-server latency.
3. **System architecture** — One block diagram with the ESP32-S3 edge boundary (INMP441 → PCM capture/ring buffer → required MFCC front end → custom INT8 KWS → activation-gated audio stream) and backend boundary (WebSocket → PCM decoder/stream receiver → ASR). Mark the optional MAX98357A return-audio path as implemented in firmware but not acoustically validated.
4. **Custom model and firmware inference** — Separate the ML-team custom “Hello Tors” model/export path from on-device inference. Identify TFLite Micro and the exact required feature path; do not imply that raw PCM is the model input or that the project re-created the ML team's training work. Show the 30-ms analysis frame, 20-ms hop, and 49 × 40 feature tensor without turning the slide into an MFCC tutorial.
5. **Hardware and audio transport** — ESP32-S3 N16R8, INMP441, Wi-Fi/WebSocket, and optional MAX98357A; 16 kHz, mono, signed PCM16; 20-ms frames (320 samples / 640-byte audio payload) plus the 16-byte application header; 800-ms prebuffer; energy-based endpoint. Note that the prebuffer may include the keyword and transport is uncompressed PCM.
6. **Feasibility, current status, and value** — Summarize what is implemented (local custom KWS integration, trigger-controlled streaming, diagnostics and local recording) alongside the intended reduction in continuous audio transmission. State clearly that competition-limit compliance and full field performance are not established.
7. **Technical supplement: resource footprint** — A compact table of model flash bytes, arena reservation, current internal static data+BSS, external static BSS/PSRAM, and app binary size. Explain that the arena is included in internal static BSS and must not be counted twice. Compare with the 256-KB requirement without implying compliance; exclude older-build heap snapshots from current-build totals.
8. **Technical supplement: evidence and next validation** — Separate completed host/build/protocol checks from pending physical-device measurements: true-positive rate, false activations/hour, idle CPU, microphone/noise robustness, keyword-end-to-server p50/p95/p99, and acoustic speaker output. Add a short validation sequence and concise references to the project contract, ML deployment review, and SIH problem statement.

## Verified implementation facts and evidence rules

- The current firmware embeds the custom 36,744-byte INT8 “Hello Tors” TFLite model and invokes it using TensorFlow Lite Micro. Model input is `[1,49,40]`; the required MFCC frontend is part of the deployed inference path.
- The edge implementation uses ESP-IDF and TensorFlow Lite Micro; identify these as open-source frameworks. The custom phrase is “Hello Tors,” not a generic assistant keyword. Attribute the supplied/trained model to the ML-team artifact rather than claiming this firmware repository contains its training pipeline.
- Current decision configuration is score threshold 0.45, two consecutive evaluated windows, and a one-second cooldown. This is a firmware setting, not a demonstrated accuracy/false-activation result on the INMP441.
- Capture/upload contract is 16-kHz mono PCM16, one 20-ms frame per message, with a 16-byte header and 640-byte payload. The current online prebuffer is 800 ms. Audio is sent only after local KWS activation; the energy endpoint is not a trained VAD.
- Current build documentation reports 16-MB flash, 8-MB PSRAM, 163,668 bytes internal static data+BSS, 96,064 bytes external static BSS, a 114,688-byte reserved internal tensor arena, and a 1,227,280-byte app binary. Together, internal and external static data+BSS are 259,732 bytes, before dynamic allocations; state that these are different memory regions and that the arena reservation is already within internal static BSS. Do not treat this static-only sum as the complete runtime footprint.
- An earlier runtime sample reports arena use of 108,412/114,688 bytes. Since its linked static footprint does not match the latest documented build, do not label it as the current build's arena-use or RAM-headroom measurement.
- The supplied feature configuration is 16 kHz PCM16 normalized by 32768, 480-sample periodic-Hann analysis frames, 320-sample hop, 512-point FFT magnitude, 40 HTK mel bands, log-mel plus the exported DCT, and INT8 quantization. Firmware adds no gain, AGC, denoising, or noise conditioning before inference; AC-RMS is used only for endpointing.
- The available pasted board log is from a different/earlier linked footprint (163,100 bytes internal static BSS and 53,680 bytes external static BSS) than the latest documented build. Do not combine that log's live heap values with the latest build's static values or present it as a latest-build measurement. Omit live headroom from the primary claims unless it is labeled as that earlier sample.
- A runtime snapshot may be presented only with its context and definitions (internal heap allocated/free, internal static data+BSS, PSRAM heap allocated/free, and external static BSS). Do not add heap-free bytes as “consumed,” double-count the arena, or describe linked static memory as live free heap. Any combined total must state which regions, build and snapshot it covers.
- Host feature/model-vector checks and protocol/backend tests are not physical microphone accuracy tests. The local playback test server is ASR-free; the separate backend source contains an ASR worker. Do not claim a current end-to-end ASR latency result without server-receipt evidence.
- Idle CPU under 10%, total RAM under 256 KB, true-positive rate, false activations/hour, end-to-end latency percentiles, and acoustic MAX98357A playback are not demonstrated in the project evidence reviewed for this deck. Present them as open evaluation items.

## Visual and editorial direction

- Match the Dhvani reference's wide 20 × 11.25-inch canvas and SIH-style story progression, but follow the supplied brief: white/light background, restrained navy/slate neutral palette, one small accent color, simple rectangular blocks/arrows, clean tables, generous spacing.
- Build diagrams, labels, and tables as editable PowerPoint shapes/text. No decorative stock images, gradients, fake dashboard screenshots, or marketing claims.
- Keep each slide to one point. Use exact technical terms and short labels. Add a subtle evidence/status label where a claim is implemented, host-tested, hardware-tested, or still pending.
- Do not include unsupported numbers, team identifiers/names, or claims copied from Dhvani. Cite project documents in a small source footer or on slide 8.

## Acceptance criteria

- Exactly eight slides, with the first six forming a coherent SIH presentation and the last two providing optional technical evidence.
- All slides accurately identify PS 26172 and do not confuse it with Dhvani's ANC problem.
- Architecture separates on-device KWS from backend ASR and shows that audio upload is activation-gated.
- Resource and evaluation slides distinguish targets from measured facts and avoid double counting.
- PowerPoint content is editable, readable, restrained, and consistent with the visual brief.
