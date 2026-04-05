# Offline Video Rendering: Feasibility Report

**Phase:** 12 (offline video exploration)  
**Date:** 2026-04-02  
**Status:** Complete

## Summary

This phase explored whether Pulp can support deterministic offline video-style
rendering without overclaiming a shipped feature. The honest outcome is:

**Recommendation: Conditional Go, library-first.**

Pulp can already support a narrow, truthful offline-rendering floor:
- deterministic PNG-sequence export on macOS
- explicit transport-aware offline audio stepping
- a clear path to future encoder integration

Pulp does **not** yet ship:
- MP4 / ProRes encoding
- cross-platform offline capture
- proven GPU-faithful headless capture parity with the live macOS GPU host

So the right next step, if product demand exists later, is a **library-first
PNG-sequence API**, not a broad `render-video` feature claim.

---

## What Was Tested

### 1. Deterministic frame-sequence export

Verified with a new macOS screenshot test:
- `test/test_screenshot.cpp`
- `Screenshot can export a deterministic PNG sequence`

The proof:
- creates a `View` tree with a layout-sized `Knob`
- attaches a `FrameClock`
- advances the clock at a fixed 30 fps step
- sweeps the knob value deterministically across frames
- writes `frame-0.png`, `frame-1.png`, `frame-2.png`
- verifies the PNG payload changes across the sequence

This is enough to prove that the existing screenshot path can emit a real PNG
sequence, not just single stills. It also captures an important renderer truth:
the offscreen screenshot path paints the laid-out tree, so tests need
layout-facing preferred sizes rather than relying on ignored absolute child
bounds.

### 2. Offline audio stepping with explicit transport/timeline context

Verified with a new headless-format test:
- `test/test_headless.cpp`
- `HeadlessHost accepts explicit transport context for offline stepping`

The proof:
- extends `HeadlessHost::process(...)` with an explicit `ProcessContext`
- preserves caller-specified timeline fields such as:
  - `is_playing`
  - `tempo_bpm`
  - `position_beats`
  - `position_samples`
  - time signature
- defaults `sample_rate` and `num_samples` from the prepared host when unset

This is the missing seam Phase 12 needed for frame-by-frame audio/video
lockstep experiments.

### 3. Existing capture/backend seams

Codebase truth after this phase:

- **Headless capture:** `render_to_png()` / `render_to_file()`
- **Live GPU capture:** `WindowHost::capture_png()` on macOS GPU host
- **Deterministic time source:** `FrameClock`
- **Offline processing:** `HeadlessHost`

These are enough for a narrow exploration, but not enough to claim a full
offline video product.

---

## What Is Feasible Today

### Feasible now

- deterministic PNG-sequence export on macOS
- explicit frame-by-frame audio stepping with host-provided transport context
- library-driven offline rendering experiments
- future CLI work built on top of these lower-level seams

### Partially feasible

- GPU-faithful capture
  - macOS has a real live GPU readback path via the window host
  - headless screenshot capture exists, but is not the same proof as final
    live GPU presentation

### Not feasible today

- in-tree MP4 encoding
- in-tree ProRes encoding
- Windows/Linux offline capture parity
- a truthful “offline video feature” claim in docs or CLI

---

## Current Architecture Reality

### Capture paths

1. **Headless screenshot path**
   - `render_to_png()`
   - `render_to_file()`
   - deterministic and simple
   - macOS-only in practice today

2. **Live GPU capture path**
   - `WindowHost::capture_png()`
   - closest to final presentation fidelity
   - tied to the live macOS window host, not a generic offline renderer

### Audio stepping path

- `HeadlessHost` is now sufficient for explicit offline stepping because the
  caller can provide `ProcessContext` directly.
- This unlocks future render loops of the form:
  - advance audio by one frame worth of samples
  - advance UI time by one frame
  - render PNG

### Encoder path

No encoder abstraction exists today. Viable future options remain:
- FFmpeg pipe/output integration
- AVFoundation writing on macOS
- raw frame sequence only

This phase does not add any encoder dependency.

---

## Pros

- perfect frame determinism for the proven screenshot path
- exact audio/visual timeline control is now possible at the host API layer
- arbitrary output resolution remains possible through screenshot dimensions
- builds on already-merged validation and screenshot infrastructure
- useful for demo generation, visual regression artifacts, and educational
  exports even before “video encoding” exists

## Cons

- screenshot implementation is still effectively macOS-first
- live GPU fidelity is not yet standardized as a headless contract
- no encoder means PNG sequences can be storage-heavy
- full productization would need more work around animation/timeline packaging,
  capture fidelity, and encoding UX

---

## Honest Blockers

1. **Cross-platform capture parity**
   - non-Apple screenshot backends are still stubbed

2. **GPU-faithful offscreen capture**
   - live GPU capture exists on macOS
   - generic headless GPU parity is still not proven

3. **No encoder abstraction**
   - MP4 / ProRes remain future integrations, not current capability

4. **Exploration only**
   - this phase proves feasibility seams
   - it does not justify marketing or docs that imply offline video already
     ships

---

## Recommendation

### Decision

**Proceed as a library-first follow-up only if there is real demand.**

That follow-up should be scoped to:
- macOS first
- PNG sequence export first
- encoder integration later

### Do not do next

- do not add a broad `pulp render-video` CLI yet
- do not promise MP4/ProRes
- do not claim cross-platform offline capture

### Best next step if revisited

1. Wrap the proven seams in a small `OfflineRenderer` library API:
   - width / height / fps / duration
   - explicit `ProcessContext` stepping
   - PNG-sequence output
2. Prove whether offscreen GPU readback can match live GPU output closely
   enough for visual trust.
3. Only then consider optional encoder integration.

---

## Code Deliverables

| File | Purpose |
|------|---------|
| `core/format/include/pulp/format/headless.hpp` | explicit transport-aware offline process overloads |
| `core/format/src/headless.cpp` | context-aware offline process implementation |
| `test/test_headless.cpp` | explicit offline transport/timeline stepping proof |
| `test/test_screenshot.cpp` | deterministic macOS PNG-sequence proof |
| `planning/v3-phase-status.md` | status/doc truth for Phase 12 |
| `docs/reports/offline-video-feasibility.md` | this report |
