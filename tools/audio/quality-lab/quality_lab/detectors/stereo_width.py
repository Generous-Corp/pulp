"""Stereo-image degradation — width collapse and phase damage.

The mono detectors (centroid/fizz/flux/transient) are blind to the stereo field: a
process can narrow the image toward mono, or flip the channels out of phase (wrecking
mono-compatibility), without changing the mono spectrum at all. This detector operates
directly on (N, 2) stereo arrays — it is NOT part of the mono `run` pipeline (which
downmixes), and is meant for validating stereo-affecting DSP (wideners, reverb,
stereo time-stretch).

It measures the relative drop in width ratio (RMS side / RMS mid) of candidate vs
reference, and watches inter-channel correlation for a phase inversion. A width DROP
(narrowing/collapse) or a sign-flip to anti-phase is the defect; widening alone is not
flagged as "worse".
"""
from __future__ import annotations

import numpy as np

from ..dsp import interchannel_correlation, stereo_width_ratio
from ..schema import DetectorResult

TOLERANCE_CLASS = "stereo_width.v1"


def detect(
    reference,
    candidate,
    sr: int,
    onset_pairs=None,
    fire_threshold_rel: float = 0.25,
) -> DetectorResult:
    """Relative stereo-width reduction of candidate vs reference, in [0, 1]. scalar =
    max(0, (w_ref - w_cand) / w_ref). fired when width collapses past the threshold OR
    the channels go anti-phase (correlation flips strongly negative when the reference
    was in-phase). Both inputs are (N, 2) stereo arrays; they are truncated to the
    shorter length. onset_pairs ignored (global)."""
    ref = np.asarray(reference, dtype=np.float64)
    cand = np.asarray(candidate, dtype=np.float64)
    n = min(len(ref), len(cand))
    ref, cand = ref[:n], cand[:n]

    w_ref = stereo_width_ratio(ref)
    w_cand = stereo_width_ratio(cand)
    rel_drop = 0.0 if w_ref <= 1e-9 else max(0.0, (w_ref - w_cand) / w_ref)

    corr_ref = interchannel_correlation(ref)
    corr_cand = interchannel_correlation(cand)
    phase_inverted = corr_cand < -0.5 and corr_ref > 0.0

    fired = rel_drop >= fire_threshold_rel or phase_inverted
    note = (f"width {w_ref:.3f}->{w_cand:.3f} (drop {rel_drop:.0%}); "
            f"corr {corr_ref:+.2f}->{corr_cand:+.2f}")
    if phase_inverted:
        note += " [PHASE INVERTED]"
    return DetectorResult(
        name="stereo_width",
        scalar=rel_drop,
        unit="stereo_width_reduction",
        fired=fired,
        time_domain="aligned",
        measured=1,
        expected=1,
        tolerance_class=TOLERANCE_CLASS,
        notes=note,
    )
