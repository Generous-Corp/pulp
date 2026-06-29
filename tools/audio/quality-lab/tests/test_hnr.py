"""HNR detector — added noise / roughness on tonal material.

`spectral_flux` catches temporal churn; HNR catches tonal *purity* loss (a different
axis). It must fire when broadband noise is mixed into a sustained tone, stay quiet on
an identity render, and not false-fire on a mere brightness change (dulling), which
removes energy without adding noise.
"""
from __future__ import annotations

from quality_lab import generate, pipeline
from quality_lab.detectors import hnr
from quality_lab.dsp import harmonic_to_noise_ratio_db

TONAL = pipeline.TONAL_CASE
SR = 48000


def _det(report, name):
    return next(d for d in report["detectors"] if d["name"] == name)


def test_hnr_drops_when_noise_is_added_to_a_tone():
    """The core metric: a clean tone has higher HNR than the same tone + broadband noise."""
    clean, _ = generate.render_tonal(SR, 2.5, 0)
    noisy = generate.noisy(clean, SR, amount=0.10)
    assert harmonic_to_noise_ratio_db(clean, SR) > harmonic_to_noise_ratio_db(noisy, SR) + 2.0


def test_detector_fires_on_added_noise_quiet_on_identity():
    clean, _ = generate.render_tonal(SR, 2.5, 0)
    noisy = generate.noisy(clean, SR, amount=0.10)
    fired = hnr.detect(clean, noisy, SR)
    assert fired.fired and fired.scalar >= 3.0, f"HNR missed added noise: {fired.scalar}"
    same = hnr.detect(clean, clean, SR)
    assert not same.fired and same.scalar == 0.0


def test_tonal_noisy_fires_hnr_via_pipeline():
    """End to end through the tonal family: the `noisy` degradation fires hnr."""
    d = _det(pipeline.run("noisy", case=TONAL), "hnr")
    assert d["fired"] is True, f"hnr missed pipeline noise: {d['scalar']}"


def test_tonal_identity_hnr_quiet():
    d = _det(pipeline.run("identity", case=TONAL), "hnr")
    assert d["fired"] is False and d["scalar"] == 0.0


def test_hnr_does_not_false_fire_on_dulling():
    """Dulling removes high-frequency energy but adds no noise — HNR should stay quiet
    (that artifact is spectral_centroid's job, not HNR's)."""
    d = _det(pipeline.run("dull", case=TONAL), "hnr")
    assert d["fired"] is False, f"hnr false-fired on dulling: {d['scalar']}"
