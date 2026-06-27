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

from ..schema import DetectorResult, WorstRegion

TOLERANCE_CLASS = "transient_sharpness.v1"


def _highband(y: np.ndarray) -> np.ndarray:
    """Cheap high-pass via first difference — emphasizes attack edges (>~300 Hz)."""
    return np.diff(np.asarray(y, dtype=np.float64), prepend=0.0)


def _smooth_env(seg: np.ndarray, sr: int, hop_s: float = 0.00025, smooth_s: float = 0.001):
    """Smoothed high-band energy envelope + hop length (samples). Smoothing is what
    keeps the rise measure stable on noise-based attacks (snare/hat)."""
    hb = _highband(seg)
    hop = max(1, int(hop_s * sr))
    win = max(1, int(smooth_s * sr))
    if hb.size < hop + win + 1:
        return np.zeros(0), hop
    e2 = hb * hb
    env = np.array(
        [np.sqrt(np.mean(e2[i : i + win]) + 1e-20) for i in range(0, len(e2) - win, hop)]
    )
    return env, hop


def _attack_rise(seg: np.ndarray, sr: int) -> float:
    """Attack sharpness of an anchored window = amplitude / 10-90% rise time of the
    smoothed high-band energy envelope (energy per sample). Higher = sharper."""
    env, hop = _smooth_env(seg, sr)
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


def _aligned_pair(
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    ref_t: float,
    cand_t: float,
    pre_ms: float = 6.0,
    post_ms: float = 26.0,
    search_ms: float = 12.0,
):
    """Extract the reference attack window and the best-matching candidate window of
    the SAME length, via local cross-correlation of the high-band signal. Returns
    (ref_seg, cand_seg) or (None, None) at the signal boundary.
    """
    pre = int(pre_ms * sr / 1000.0)
    post = int(post_ms * sr / 1000.0)
    search = int(search_ms * sr / 1000.0)
    rc = int(round(ref_t * sr))
    cc = int(round(cand_t * sr))
    if rc - pre < 0 or rc + post > len(reference):
        return None, None
    ref_seg = reference[rc - pre : rc + post]
    c0 = max(0, cc - pre - search)
    c1 = min(len(candidate), cc + post + search)
    cwin = candidate[c0:c1]
    if len(cwin) < len(ref_seg):
        return None, None
    xc = np.correlate(_highband(cwin), _highband(ref_seg), mode="valid")
    best = int(np.argmax(xc))
    cand_seg = cwin[best : best + len(ref_seg)]
    if len(cand_seg) != len(ref_seg):
        return None, None
    return ref_seg, cand_seg


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
        ref_seg, cand_seg = _aligned_pair(reference, candidate, sr, ref_t, cand_t)
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
        curve=curve,
        worst_regions=worst[:3],
        tolerance_class=TOLERANCE_CLASS,
        notes="per-onset high-band attack-rise deficit vs locally aligned reference",
    )


# Back-compat alias for the P0a sanity test.
def _attack_slope(y: np.ndarray, sr: int, onset_s: float, win_ms: float = 24.0) -> float:
    c = int(round(onset_s * sr))
    pre = int(0.006 * sr)
    post = int(win_ms * sr / 1000.0)
    lo, hi = max(0, c - pre), min(len(y), c + post)
    return _attack_rise(y[lo:hi], sr)
