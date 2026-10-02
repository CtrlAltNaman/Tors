# EdgeAIKWS Eight-Slide Editable Template Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a separate, editable eight-slide EdgeAIKWS PowerPoint template using the approved Dhvani-inspired placements and the user's exact slide order.

**Architecture:** Generate a 20 × 11.25-inch PowerPoint with `python-pptx`; use native editable shapes, text boxes, tables, connectors, and image frames. Keep all new source/output names distinct from the existing project and reference decks. Validate slide count, headings, shape editability, and rendered appearance.

**Tech Stack:** Python 3.12, `python-pptx`, PowerPoint COM export for visual inspection.

---

### Task 1: Build the editable eight-slide generator

**Files:**
- Create: `scripts/generate_edgeaikws_8slide_template.py`
- Create: `tests/test_edgeaikws_8slide_template.py`
- Create: `EdgeAIKWS_SIH26172_8Slide_Template.pptx`
- Reference only: `Dhvani_Kavach_SIH26052.pptx`
- Preserve: `EdgeAIKWS_SIH26172.pptx`

- [x] **Step 1: Write the artifact acceptance test first.**
  - Add `tests/test_edgeaikws_8slide_template.py` asserting slide count, canvas dimensions, requested heading order, placeholder/editable shape presence, and absence of Dhvani-specific branding/content.
  - Run: `python -m unittest discover -s tests -p test_edgeaikws_8slide_template.py -v`
  - Expected before generation: FAIL because the requested output file does not yet exist.
- [x] **Step 2: Implement a focused generator.**
  - Set 16:9 dimensions to 20 × 11.25 inches.
  - Use reusable helpers for rectangles, text, connectors, tables, image frames, page headers, and footers.
  - Create exactly eight slides in the approved order, matching the slide-by-slide layout in `docs/superpowers/specs/2026-09-29-edgeaikws-8slide-template-design.md`.
  - Keep content fields as visibly editable `[Add ...]` prompts; do not insert claims, numeric values, external references, fake screenshots, or Dhvani branding.
  - Write only to `EdgeAIKWS_SIH26172_8Slide_Template.pptx`.
- [x] **Step 3: Generate the PowerPoint.**
  - Run: `python scripts/generate_edgeaikws_8slide_template.py`
  - Expected: exit code 0 and the new PPTX at the output path.
- [x] **Step 4: Verify structure and content.**
  - Open the output with `python-pptx` and assert eight slides, 20 × 11.25-inch canvas, requested slide heading order, and no Dhvani-specific strings such as `Dhvani`, `DCCRN`, or `ANC`.
  - Confirm each slide has multiple editable shapes and the output differs from, and does not replace, the pre-existing deck.
- [x] **Step 5: Render and inspect every slide.**
  - Export all slides to PNG using PowerPoint COM into a task-specific directory under `$env:TEMP`.
  - Inspect all eight renders for clipping, overlaps, legibility, consistent footer/header placement, and usable placeholder dimensions.
  - If an issue appears, adjust the generator with `apply_patch`, regenerate, and rerun structural and visual checks.

## Self-review

- All eight requested sections are represented by Task 1's explicit slide sequence.
- PDF six-slide compliance is intentionally out of scope because the user selected only the eight-slide working version; this limitation is disclosed in the design specification.
- All generated content remains editable and no unsupported metrics or factual claims are added.
- Visual validation covers all slides, and the existing presentation files are preserved.
