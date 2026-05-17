"""Pulp Motion visual-analysis pipeline.

Operates on a directory of sequential PNG frames (any source: a host
window-capture loop, scripted `ui-preview` frame dumps, an
external `ffmpeg` extraction) and produces a structured report:

  - `analysis.json`  — machine-readable per-frame metrics
  - `summary.md`     — agent-readable Markdown summary
  - `diff/*.png`     — pairwise pixel-diff heatmaps
  - `keyframes.png`  — keyframe sprite for the summary

See `analyze_sequence.py` for the entry point and CLI.
"""
