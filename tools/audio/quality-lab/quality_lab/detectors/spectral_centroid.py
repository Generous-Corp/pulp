"""Spectral-centroid (brightness) deviation (§6 detector).

The stretch skill records that the spectral centroid was the metric that reliably
caught STN noise-morphing *dulling* (~400 centroid points muddier) when peak/RMS
could not. This detector compares the candidate's long-term-average-spectrum centroid
to the reference's and reports the relative shift — negative = duller, positive =
brighter. It is global (no alignment), so it is robust on any material.

Time domain: aligned (a global LTAS over the whole level-matched signal); there is no
per-region localization — brightness change is a whole-render property.
"""
from __future__ import annotations

import numpy as np

from ..dsp import relative_centroid_shift
from ..schema import DetectorResult

TOLERANCE_CLASS = "spectral_centroid.v1"


def detect(
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    onset_pairs: list[tuple[float, float]] | None = None,
    fire_threshold_rel: float = 0.05,
) -> DetectorResult:
    """Relative centroid shift of candidate vs reference. scalar = |relative shift|;
    fired = scalar >= threshold (default 5%). onset_pairs is ignored (global metric)."""
    rel, c_ref, c_cand = relative_centroid_shift(reference, candidate, sr)
    direction = "duller" if rel < 0 else "brighter"
    return DetectorResult(
        name="spectral_centroid",
        scalar=abs(rel),
        unit="rel_centroid_shift",
        fired=abs(rel) >= fire_threshold_rel,
        time_domain="aligned",
        measured=1,
        expected=1,
        tolerance_class=TOLERANCE_CLASS,
        notes=f"LTAS centroid {c_ref:.0f}->{c_cand:.0f} Hz ({direction} {abs(rel)*100:.1f}%)",
    )
