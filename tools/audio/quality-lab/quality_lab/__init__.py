"""Audio Quality Lab — perception-aware offline harness so agents can "hear".

Phase P0a: the smallest end-to-end slice (generate -> level-match -> onset-map align
-> transient-sharpness detector -> report) proving the architecture before the corpus
is built. See planning/2026-06-26-audio-quality-lab-perceptual-harness.md.

This package is an additive, opt-in developer/CI tool. It is never linked into the
MIT core or a shipped plugin; FFT/analysis stays tool-side.
"""

__version__ = "0.0.1-p0a"
