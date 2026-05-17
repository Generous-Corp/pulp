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
| `--max-diff-frames N` | `8` | Cap on emitted diff PNGs; `0` = unlimited (all pairs) |
| `--grid` | off | Emit alphanumeric grid overlays (A1..H12) on `grid/`, `diff_grid/`, and `keyframes_grid.png` so claims can cite a cell |
| `--grid-rows N` | `8` | Grid overlay rows (A..Z) |
| `--grid-cols N` | `12` | Grid overlay columns (1..N) |
| `--grid-theme` | `auto` | `auto` (samples corner luminance) \| `light` \| `dark` |
| `--trim` | off | Drop idle prefix + suffix from the analysis window (frames stay on disk; `summary.md` reports `trimmed_leading_frames` / `trimmed_trailing_frames`) |
| `--trim-threshold F` | `0.01` | Mean-diff luminance fraction for `--trim` |
| `--affine` | off | Estimate translation / rotation / scale first→last (opencv if installed, else PIL-FFT translation only — see `requirements-optional.txt`); emits `analysis.json#affine_first_to_last` and a `## Net motion` section in `summary.md` |

## Motion-gated capture

```bash
# macOS window region (requires --bounds X,Y,W,H)
python3 tools/motion/visual/capture_sim_frames.py \
    --source macos --bounds 0,0,800,600 \
    --output-dir ./captures/card-open/ \
    --fps 30 --frame-count 60 \
    --gate-threshold 4.0 --gate-consecutive 1 \
    --idle-timeout 8

# Booted iOS Simulator
python3 tools/motion/visual/capture_sim_frames.py \
    --source simulator \
    --output-dir ./captures/card-open/ \
    --fps 30 --frame-count 60
```

`capture_sim_frames.py` only starts saving frames once real motion appears,
so a short pre-roll doesn't pollute the analysis window. It exits 3 (CTest
SKIP) when neither `screencapture` nor a booted simulator is available, so
it composes cleanly with CI lanes that lack the platform tooling. Pair with
XcodeBuildMCP on macOS / iOS for log capture and screenshot orchestration.

## Claim-evidence preamble

`summary.md` opens with a contract every claim made from the report must
satisfy: (1) cite pair index (`NN→NN+1`), (2) name the artifact used
(`frames/`, `diff/`, `grid/`, `diff_grid/`, `keyframes.png`, or
`affine_first_to_last`), and (3) cite a `pairs[].confidence` /
`summary.mean_confidence` score `0.0..1.0`. **Confidence < 0.7 means the
analyzer is unsure** — escalate by re-running with `--max-diff-frames 0` (all
pairs), a longer capture window, or fall back to a runtime trace if
instrumentation is possible.

## Self-check

```bash
python3 tools/motion/visual/test_self_check.py    # baseline + claim-evidence
python3 tools/motion/visual/test_grid_overlay.py  # --grid / --trim / --affine
python3 tools/motion/visual/test_capture_smoke.py # gated capture (skip 3 w/o source)
```

`test_self_check.py` synthesizes a 6-frame sliding-rectangle sequence, runs
the analyzer, asserts artifact presence and JSON shape. All three tests exit
with `3` if Python dependencies aren't installed (so CTest skips cleanly).

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
