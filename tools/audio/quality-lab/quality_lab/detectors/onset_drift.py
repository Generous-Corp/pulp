"""Onset-timing drift (§6 detector #4).

Hits landing late or early relative to the source groove — a different artifact class
from transient *softening*, proving the harness generalizes past one detector with
little new code. Per matched onset we take the local cross-correlation lag (the exact
candidate offset of that attack), subtract the global median lag (constant
latency / time-map offset), and report the residual as drift in milliseconds. A
faithful render drifts ~0 ms everywhere; a time-jittered hit reads its true offset.

Time domain: source-time — drift is meaningful only against where the hit *should*
land, so it is measured against the reference onset grid, not the de-drifted audio.
"""
from __future__ import annotations

import numpy as np

from ..dsp import highband
from ..schema import DetectorResult, WorstRegion

TOLERANCE_CLASS = "onset_drift.v1"


def _body_offset_ms(
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    ref_t: float,
    nominal_latency_s: float,
    body_start_ms: float = 12.0,
    body_len_ms: float = 45.0,
    search_ms: float = 18.0,
) -> float | None:
    """Offset (ms) of the candidate hit BODY relative to the reference hit body.

    Correlates a post-attack body window of the reference against the candidate around
    the nominally-expected position (reference grid + a coarse global latency). The
    body/tail is intact under an attack-only smear, so a smear gives offset == latency
    (no drift), while a whole-hit time shift moves the body and shows up here.
    """
    bs = int(body_start_ms * sr / 1000.0)
    bl = int(body_len_ms * sr / 1000.0)
    search = int(search_ms * sr / 1000.0)
    r0 = int(round(ref_t * sr)) + bs
    r1 = r0 + bl
    if r0 < 0 or r1 > len(reference):
        return None
    ref_body = highband(reference[r0:r1])
    center = int(round((ref_t + nominal_latency_s) * sr)) + bs
    c0 = max(0, center - search)
    c1 = min(len(candidate), center + bl + search)
    cwin = highband(candidate[c0:c1])
    if len(cwin) < len(ref_body) or np.allclose(ref_body, 0.0):
        return None
    xc = np.correlate(cwin, ref_body, mode="valid")
    best = int(np.argmax(xc))
    abs_body_start = c0 + best
    return (abs_body_start - r0) * 1000.0 / sr  # candidate body pos vs reference = latency(+drift)


def detect(
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    onset_pairs: list[tuple[float, float]],
    fire_threshold_ms: float = 3.0,
) -> DetectorResult:
    """Per-onset timing drift (ms) of candidate vs reference after removing the global
    offset. scalar = worst |residual drift| in ms; fired = scalar >= threshold."""
    if not onset_pairs:
        nominal = 0.0
    else:
        nominal = float(np.median([c - r for r, c in onset_pairs]))

    times: list[float] = []
    offsets_ms: list[float] = []
    for ref_t, _cand_t in onset_pairs:
        off = _body_offset_ms(reference, candidate, sr, ref_t, nominal)
        if off is None:
            continue
        times.append(ref_t + off / 1000.0)
        offsets_ms.append(off)

    curve: list[tuple[float, float]] = []
    worst: list[WorstRegion] = []
    scalar = 0.0
    if offsets_ms:
        global_offset = float(np.median(offsets_ms))
        for t, off in zip(times, offsets_ms):
            drift = off - global_offset
            curve.append((t, drift))
            worst.append(
                WorstRegion(
                    time_s=t,
                    severity=abs(drift),
                    detector="onset_drift",
                    label=f"onset {'late' if drift > 0 else 'early'} {abs(drift):.1f} ms",
                )
            )
        scalar = max((abs(d) for _, d in curve), default=0.0)
    worst.sort(key=lambda w: w.severity, reverse=True)
    return DetectorResult(
        name="onset_drift",
        scalar=scalar,
        unit="ms",
        fired=scalar >= fire_threshold_ms,
        time_domain="source-time",
        curve=curve,
        worst_regions=worst[:3],
        tolerance_class=TOLERANCE_CLASS,
        notes="per-onset cross-correlation lag minus global median offset",
    )
