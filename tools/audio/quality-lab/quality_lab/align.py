"""Alignment — the prerequisite layer that runs *before* any detector (§4.5.1).

Reference and candidate differ in length / latency / time-map (a stretch is the
worst case). The detectors must never do their own ad-hoc trimming; they consume the
onset map this layer produces. P0a implements onset detection + an onset-map (match
attacks ref<->candidate). ratio-map / fixed-latency-trim / constrained-DTW are later
policies that plug in here behind the same interface.
"""
from __future__ import annotations

import numpy as np


def onset_envelope(
    y: np.ndarray, sr: int, win: int = 256, hop: int = 128
) -> tuple[np.ndarray, np.ndarray]:
    """Half-wave-rectified short-time energy flux + the hop times (seconds)."""
    y = np.asarray(y, dtype=np.float64)
    n = max(0, (len(y) - win) // hop + 1)
    e = np.empty(n, dtype=np.float64)
    for i in range(n):
        s = i * hop
        e[i] = np.sqrt(np.mean(y[s : s + win] ** 2) + 1e-20)
    flux = np.maximum(0.0, np.diff(e, prepend=e[0] if n else 0.0))
    times = (np.arange(n) * hop + win / 2.0) / sr
    return flux, times


def detect_onsets(
    y: np.ndarray,
    sr: int,
    win: int = 256,
    hop: int = 128,
    min_gap_s: float = 0.05,
    thresh_rel: float = 0.15,
) -> list[float]:
    """Peak-pick the flux above an adaptive threshold with a refractory gap.

    Returns onset times in seconds. Deterministic; documented params are part of the
    analyzer determinism contract (§4.5.3).
    """
    flux, times = onset_envelope(y, sr, win, hop)
    if flux.size == 0:
        return []
    thresh = thresh_rel * float(flux.max())
    min_gap = int(min_gap_s * sr / hop)
    onsets: list[float] = []
    last = -(10**9)
    for i in range(1, len(flux) - 1):
        if (
            flux[i] >= thresh
            and flux[i] >= flux[i - 1]
            and flux[i] >= flux[i + 1]
            and (i - last) >= min_gap
        ):
            onsets.append(float(times[i]))
            last = i
    return onsets


def map_onsets(
    src_onsets: list[float], cand_onsets: list[float], src_dur: float, cand_dur: float
) -> list[tuple[float, float]]:
    """Match source onsets to candidate onsets by nearest position in *normalized*
    time (0..1), monotonically. Handles the different-length / different-time-map case
    a stretch creates without assuming equal counts.
    """
    if not src_onsets or not cand_onsets:
        return []
    sd = src_dur or 1.0
    cd = cand_dur or 1.0
    pairs: list[tuple[float, float]] = []
    j0 = 0
    for s in src_onsets:
        if j0 >= len(cand_onsets):
            break  # ran out of candidate onsets — remaining source onsets are unmatched
        sn = s / sd
        best_j, best_d = j0, None
        for j in range(j0, len(cand_onsets)):
            d = abs(cand_onsets[j] / cd - sn)
            if best_d is None or d < best_d:
                best_d, best_j = d, j
            elif cand_onsets[j] / cd - sn > 0 and d > best_d:
                break
        pairs.append((s, cand_onsets[best_j]))
        j0 = best_j + 1  # strict advance — never reuse a candidate onset
    return pairs
