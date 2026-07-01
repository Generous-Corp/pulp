"""High-frequency "fizz" / metallic-sizzle detector (§6 detector).

Phase vocoders and noise-morphing can birth high-frequency energy that wasn't in the
source — heard as metallic shimmer / sizzle / fizz. This detector compares the
fraction of LTAS energy above a cutoff (default 8 kHz) in candidate vs reference;
a positive delta means the candidate added HF energy the source didn't have.

Global (no alignment), scale-invariant (a ratio of energies), so robust on any
material. Time domain: aligned (whole-render LTAS).
"""
from __future__ import annotations

import numpy as np

from ..dsp import hf_fraction_delta
from ..schema import DetectorResult

TOLERANCE_CLASS = "hf_fizz.v1"


def detect(
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    onset_pairs: list[tuple[float, float]] | None = None,
    cutoff_hz: float = 8000.0,
    fire_threshold: float = 0.02,
) -> DetectorResult:
    """Added HF-energy fraction of candidate vs reference. scalar = max(0, delta);
    fired = scalar >= threshold. onset_pairs is ignored (global metric)."""
    delta, hf_ref, hf_cand = hf_fraction_delta(reference, candidate, sr, cutoff_hz)
    added = max(0.0, delta)  # only ADDED fizz fires; losing HF is the centroid detector's job
    return DetectorResult(
        name="hf_fizz",
        scalar=added,
        unit="hf_energy_frac_delta",
        fired=added >= fire_threshold,
        time_domain="aligned",
        measured=1,
        expected=1,
        tolerance_class=TOLERANCE_CLASS,
        notes=f"HF(>={cutoff_hz:.0f}Hz) energy frac {hf_ref:.3f}->{hf_cand:.3f} (delta {delta:+.3f})",
    )
