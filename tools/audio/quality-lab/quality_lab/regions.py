"""Listenable artifacts (§5 P2 — worst-region extraction).

Turn a detector's per-region timestamps into short WAV clips a developer (or an
audio-capable model) can actually play: the full reference + candidate, and a
reference/candidate clip pair around each localized worst region. The point is to
make "attack softer at 0:02.3" not just a number but something you can hear.
"""
from __future__ import annotations

import os
from typing import Any

import numpy as np

from . import audio_io
from .schema import DetectorResult


def _clip(y: np.ndarray, sr: int, center_s: float, window_ms: float) -> np.ndarray:
    half = int(window_ms * sr / 2000.0)
    c = int(round(center_s * sr))
    lo = max(0, c - half)
    hi = min(len(y), c + half)
    return y[lo:hi]


def export_artifacts(
    out_dir: str,
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    results: list[DetectorResult],
    window_ms: float = 400.0,
    max_regions: int = 5,
) -> dict[str, Any]:
    """Write reference.wav, candidate.wav, and per-worst-region clip pairs. Returns a
    `listening` block (relative paths) for the report."""
    os.makedirs(out_dir, exist_ok=True)
    audio_io.save_wav(os.path.join(out_dir, "reference.wav"), reference, sr)
    audio_io.save_wav(os.path.join(out_dir, "candidate.wav"), candidate, sr)

    # Gather localized worst regions across detectors (only those that localize), sorted
    # by severity within each detector (already sorted), de-duplicated by ~timestamp.
    flagged: list[tuple[float, str, float, str]] = []
    for d in results:
        if not d.fired:
            continue
        for w in d.worst_regions:
            flagged.append((w.time_s, d.name, w.severity, w.label))

    regions_out: list[dict[str, Any]] = []
    seen: list[float] = []
    for time_s, det, severity, label in flagged:
        if any(abs(time_s - t) < window_ms / 2000.0 for t in seen):
            continue  # overlapping region already captured
        seen.append(time_s)
        stem = f"region-{len(regions_out):02d}-{det}-{time_s:0.3f}"
        ref_clip = _clip(reference, sr, time_s, window_ms)
        cand_clip = _clip(candidate, sr, time_s, window_ms)
        audio_io.save_wav(os.path.join(out_dir, f"{stem}.ref.wav"), ref_clip, sr)
        audio_io.save_wav(os.path.join(out_dir, f"{stem}.cand.wav"), cand_clip, sr)
        regions_out.append({
            "time_s": round(time_s, 4),
            "detector": det,
            "severity": round(severity, 4),
            "label": label,
            "reference_clip": f"{stem}.ref.wav",
            "candidate_clip": f"{stem}.cand.wav",
        })
        if len(regions_out) >= max_regions:
            break

    return {
        "dir": out_dir,
        "reference": "reference.wav",
        "candidate": "candidate.wav",
        "regions": regions_out,
        "note": "clip pairs around each localized worst region; play ref vs cand to hear it",
    }
