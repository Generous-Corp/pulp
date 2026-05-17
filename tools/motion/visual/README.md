# `tools/motion/visual/` — Pulp Motion visual-analysis pipeline

Operates on a directory of sequential PNG frames (any source: host
window capture, scripted `pulp-ui-preview` frame dumps, an external
`ffmpeg` extraction) and produces a structured report:

- `analysis.json` — machine-readable per-frame metrics
- `summary.md` — agent-readable Markdown summary
- `diff/*.png` — pairwise pixel-diff heatmaps
- `keyframes.png` — keyframe sprite for the summary

This is the pixel-truth fallback for animations whose values aren't
observable via the runtime scalar trace (Phases 0–3) — typically
transitions, GPU filters, mask compositing, anything that only shows
up as pixels.

## Install

```bash
pip install -r tools/motion/visual/requirements.txt
```

Dependencies are MIT / BSD / Apache 2.0 only.

## Use

```bash
python3 -m tools.motion.visual.analyze_sequence \
    --frames-dir ./captures/card-open/ \
    --output     ./reports/card-open/
```

Useful flags:

| Flag | Default | Notes |
|---|---|---|
| `--pattern` | `frame_*.png` | Glob for frame filenames |
| `--keyframes N` | `2` | Top-delta keyframes in addition to first/mid/last |
| `--max-diff-frames N` | `8` | Cap on emitted diff PNGs; `0` = unlimited |

## Self-check

```bash
python3 tools/motion/visual/test_self_check.py
```

Synthesizes a 6-frame sliding-rectangle sequence, runs the analyzer,
asserts artifact presence and JSON shape. Exits with `3` if Python
dependencies aren't installed (so CTest can skip cleanly).

## Output schema

`analysis.json` carries a `schema_version` field. Downstream consumers
(the assertion CLI, agent skills) read this and refuse unknown
versions. Bump the constant in `analyze_sequence.py` on breaking
changes.

## What it does NOT do

- **Scripted capture** — driving `pulp-ui-preview` to dump a
  deterministic frame sequence is a separate step. Today, capture is
  whatever produces sequential PNGs in a directory (manual `screencapture`
  loop, `ffmpeg`, or a future scripted mode).
- **Optical flow** — pixel diff + SSIM are sufficient for the current
  agent workflows; optical flow is a follow-up if SSIM ever proves
  insufficient.
- **Motion-trace correlation** — pairing scalar traces (Phase 0 sample
  events) with visual frames lives in the assertion / record-replay
  pipeline.
