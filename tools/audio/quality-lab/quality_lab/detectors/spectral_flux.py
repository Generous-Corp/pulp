"""Spectral-flux deviation — graininess / temporal instability (§6 detector).

A phase vocoder (and granular processing) can add frame-to-frame spectral churn the
source didn't have — heard as graininess / roughness, especially on SUSTAINED material.
This detector compares the mean, energy-normalized spectral flux of candidate vs
reference; a relative increase means the candidate is grainier. Global (no alignment),
level-invariant.

Material note: on transient-heavy material (drums) the onset flux dominates and this
does not discriminate graininess — it belongs to the tonal/sustained case families.
"""
from __future__ import annotations

from ..dsp import mean_spectral_flux
from ..schema import DetectorResult

TOLERANCE_CLASS = "spectral_flux.v1"


def detect(
    reference,
    candidate,
    sr: int,
    onset_pairs=None,
    fire_threshold_rel: float = 0.15,
) -> DetectorResult:
    """Relative increase in mean spectral flux of candidate vs reference. scalar =
    max(0, relative increase); fired = scalar >= threshold. onset_pairs ignored."""
    f_ref = mean_spectral_flux(reference, sr)
    f_cand = mean_spectral_flux(candidate, sr)
    rel = 0.0 if f_ref <= 1e-12 else (f_cand - f_ref) / f_ref
    added = max(0.0, rel)  # only ADDED churn is graininess; smoother-than-source isn't a defect
    return DetectorResult(
        name="spectral_flux",
        scalar=added,
        unit="rel_flux_increase",
        fired=added >= fire_threshold_rel,
        time_domain="aligned",
        measured=1,
        expected=1,
        tolerance_class=TOLERANCE_CLASS,
        notes=f"mean spectral flux {f_ref:.4f}->{f_cand:.4f} (rel {rel:+.2f})",
    )
