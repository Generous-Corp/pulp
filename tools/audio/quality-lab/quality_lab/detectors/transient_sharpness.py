"""Transient sharpness / attack-slope loss (§6 detector #1, the ★ P0a detector).

The phase vocoder smears percussion *attacks* to ~70-75% of the source peak — the
"compressed / less dynamic" artifact — and it is invisible to peak/RMS/clip (output
never clips). Per matched onset this detector locally cross-correlates the reference
attack against the candidate to lock the exact lag, extracts sample-identical windows,
and compares the high-band attack *rise* (amplitude / 10-90% rise time). It fires when
attacks are softened and is exactly quiet on a faithful (identity) render — because
aligned identical windows give an identical rise.

Time domain: onsets are matched by the alignment layer (§4.5.1); the fine per-onset
lag is refined here by cross-correlation, then the attack is measured in raw-output
time where a real softening actually happens.
"""
from __future__ import annotations

import numpy as np

from ..dsp import local_align, smooth_energy_env
from ..schema import DetectorResult, WorstRegion

TOLERANCE_CLASS = "transient_sharpness.v1"


def _attack_rise(seg: np.ndarray, sr: int) -> float:
    """Attack sharpness of an anchored window = amplitude / 10-90% rise time of the
    smoothed high-band energy envelope (energy per sample). Higher = sharper."""
    env, hop = smooth_energy_env(seg, sr)
    if env.size < 4:
        return 0.0
    peak = int(np.argmax(env))
    if peak < 1:
        return 0.0
    base = float(np.min(env[: peak + 1]))
    amp = float(env[peak]) - base
    if amp <= 1e-12:
        return 0.0
    lo_th, hi_th = base + 0.1 * amp, base + 0.9 * amp
    pre = env[: peak + 1]
    below_lo = np.where(pre <= lo_th)[0]
    below_hi = np.where(pre <= hi_th)[0]
    i_lo = int(below_lo[-1]) if below_lo.size else 0
    i_hi = int(below_hi[-1]) if below_hi.size else peak
    rise_samples = max(1, (i_hi - i_lo) * hop)
    return (0.8 * amp) / rise_samples


def detect(
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    onset_pairs: list[tuple[float, float]],
    fire_threshold: float = 0.20,
) -> DetectorResult:
    """Per-onset attack-rise deficit of candidate vs reference, after local alignment.

    `onset_pairs` are (reference_onset_s, candidate_onset_s) from the alignment layer.
    scalar = worst (max) deficit across onsets, in [0,1]; fired = scalar >= threshold.
    """
    curve: list[tuple[float, float]] = []
    worst: list[WorstRegion] = []
    for ref_t, cand_t in onset_pairs:
        ref_seg, cand_seg, _lag = local_align(reference, candidate, sr, ref_t, cand_t)
        if ref_seg is None:
            continue
        s_ref = _attack_rise(ref_seg, sr)
        s_cand = _attack_rise(cand_seg, sr)
        if s_ref <= 1e-9:
            continue
        deficit = float(np.clip(1.0 - s_cand / s_ref, 0.0, 1.0))
        curve.append((cand_t, deficit))
        worst.append(
            WorstRegion(
                time_s=cand_t,
                severity=deficit,
                detector="transient_sharpness",
                label=f"attack softer by {deficit * 100:.0f}%",
            )
        )

    scalar = max((d for _, d in curve), default=0.0)
    worst.sort(key=lambda w: w.severity, reverse=True)
    return DetectorResult(
        name="transient_sharpness",
        scalar=scalar,
        unit="deficit_0to1",
        fired=scalar >= fire_threshold,
        time_domain="raw-output",
        measured=len(curve),
        expected=len(onset_pairs),
        curve=curve,
        worst_regions=worst[:3],
        tolerance_class=TOLERANCE_CLASS,
        notes="per-onset high-band attack-rise deficit vs locally aligned reference",
    )


# Convenience for the P0a sanity test: measure one attack at an onset.
def _attack_slope(y: np.ndarray, sr: int, onset_s: float, win_ms: float = 24.0) -> float:
    c = int(round(onset_s * sr))
    pre = int(0.006 * sr)
    post = int(win_ms * sr / 1000.0)
    lo, hi = max(0, c - pre), min(len(y), c + post)
    return _attack_rise(y[lo:hi], sr)
