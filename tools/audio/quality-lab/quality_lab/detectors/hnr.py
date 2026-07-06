"""Harmonic-to-noise-ratio drop — added noise / roughness on tonal material.

A process can inject broadband noise or roughness into otherwise-harmonic content
(phase-vocoder "watery/noisy" artifacts, vocal roughness, breathiness creeping into a
held note). `spectral_flux` catches *temporal* churn; this catches *tonal purity loss*
— a different axis. It compares the mean autocorrelation HNR of candidate vs reference;
an HNR DROP means the candidate is noisier/rougher. Global (no alignment), level-
invariant. Belongs to the tonal / sustained case families (HNR is undefined on
percussive/aperiodic material).
"""
from __future__ import annotations

from ..dsp import harmonic_to_noise_ratio_db
from ..schema import DetectorResult

TOLERANCE_CLASS = "hnr.v1"


def detect(
    reference,
    candidate,
    sr: int,
    onset_pairs=None,
    fire_threshold_db: float = 3.0,
) -> DetectorResult:
    """HNR drop (reference - candidate), in dB. scalar = max(0, drop); only a DROP is a
    defect (a cleaner-than-source candidate isn't). fired = scalar >= threshold.
    onset_pairs ignored (global)."""
    hnr_ref = harmonic_to_noise_ratio_db(reference, sr)
    hnr_cand = harmonic_to_noise_ratio_db(candidate, sr)
    drop = max(0.0, hnr_ref - hnr_cand)
    return DetectorResult(
        name="hnr",
        scalar=drop,
        unit="hnr_drop_db",
        fired=drop >= fire_threshold_db,
        time_domain="aligned",
        measured=1,
        expected=1,
        tolerance_class=TOLERANCE_CLASS,
        notes=f"HNR {hnr_ref:.1f}dB -> {hnr_cand:.1f}dB (drop {drop:.1f}dB)",
    )
