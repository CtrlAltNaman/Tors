# EdgeAIKWS SIH Deck Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce an editable, eight-slide SIH jury PowerPoint for Problem Statement 26172, with six core slides and two technical supplements.

**Architecture:** Generate the deck locally with `python-pptx` as editable PowerPoint text, tables, and vector shapes. Keep the generator separate from firmware/backend code; derive content only from the approved design spec and cited project files. Use Microsoft PowerPoint to render slide images for visual review, then correct any clipping or density problems in the generator and rebuild.

**Tech Stack:** Python 3.12, python-pptx 1.0.2, Microsoft PowerPoint COM export for slide-image review.

---

## File map

- Read only: `Dhvani_Kavach_SIH26052.pptx` (narrative/layout reference), `docs/superpowers/specs/2026-09-29-edgeaikws-sih-deck-design.md`, `docs/EXACT_MODEL_INTEGRATION.md`, `docs/ESP32_BACKEND_DEVICE_CONTRACT.md`, `docs/ESP32S3_ML_DEPLOYMENT_REVIEW.md`, `docs/LOCAL_DIAGNOSTICS.md`, `docs/LED_STATES_AND_RECORDINGS.md`, `docs/SPEAKER_PLAYBACK_FIX.md`, `SERVER SIDE CODE/sih-voice-activator-main/README.md`, `ARCHITECTURE.md`, and `PROTOCOL.md`.
- Create: `scripts/generate_edgeaikws_sih_deck.py` — one focused script for layout helpers, slide content, and writing the deck.
- Create: `EdgeAIKWS_SIH26172.pptx` — final editable presentation at the project root.
- Temporary only: eight slide PNG previews under the system temporary directory; do not add previews to the repository.
- Do not modify firmware, backend implementation, model files, `record.py`, or the Dhvani source deck.

### Task 1: Build the editable deck generator

**Files:**
- Create: `scripts/generate_edgeaikws_sih_deck.py`

- [x] Define the presentation as 20 × 11.25 inches, use a white background, navy/slate text and lines with one restrained accent, and establish reusable helpers for slide title/footer, body text, boxes, arrows, and grid tables.
- [x] Implement slides 1–6: title and SIH identity; problem and metrics; edge/backend architecture; custom model and inference flow; hardware and PCM/WebSocket contract; feasibility/current implementation status.
- [x] Implement slides 7–8: current-build memory accounting; verified evidence versus unmeasured metrics and validation steps/references.
- [x] Keep diagrams and tables editable. Use a dashed or otherwise distinct connector for the optional MAX98357A acoustic output path. Do not embed stock photos, rasterized diagrams, fabricated metrics, or Dhvani content.
- [x] Keep the latest-build static memory numbers separate from the older runtime log. The 114,688-byte tensor-arena reservation is already part of the 163,668-byte internal static data+BSS; never add it again. Do not label the older 108,412-byte arena-use sample as a latest-build value.
- [x] Add the source citations and implementation-status labels specified in the approved design spec.

### Task 2: Generate the PowerPoint

**Files:**
- Create: `EdgeAIKWS_SIH26172.pptx`

- [x] Run from the project root: `python scripts/generate_edgeaikws_sih_deck.py`.
- [x] Expected result: the script reports the output path and exactly eight slides; the source/reference PPTX and all firmware/backend files remain unchanged.

### Task 3: Inspect and hand off the rendered deck

**Files:**
- Read: `EdgeAIKWS_SIH26172.pptx`
- Temporary: rendered slide PNGs under the system temporary directory.

- [x] Reopen the generated file with python-pptx and confirm it contains eight slides, the specified 20 × 11.25-inch canvas, and non-empty titles/content on all eight slides.
- [x] Export each slide to PNG using the installed Microsoft PowerPoint application and inspect the images at presentation size. Check text wrapping, contrast, alignment, margins, table density, diagram arrow direction, and source-footer legibility.
- [x] If any item clips, overlaps, or is unreadable, adjust only the generator and regenerate; repeat visual inspection until all slides are clean.
- [x] Confirm the final handoff distinguishes host/build/protocol evidence from physical-device validation and does not claim <256 KB RAM, <10% idle CPU, detection/false-activation performance, ASR latency percentiles, or verified acoustic playback.
- [x] Report the final file path and a concise verification summary. Do not flash hardware or start/change any backend service.
