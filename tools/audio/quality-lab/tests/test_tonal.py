"""Tonal QualityCase family (§3.5 generalization) + spectral_flux validation.

The drum corpus is transient-dominated, where spectral_flux can't discriminate
graininess. The TONAL family (sustained vocal/pad) is where graininess is actually
heard — and where spectral_flux must cleanly separate. This also proves the harness
generalizes beyond drums: a second family with identity alignment, run through the same
generate -> level-match -> align -> detect -> report machinery.
"""
from __future__ import annotations

from quality_lab import pipeline

TONAL = pipeline.TONAL_CASE


def _det(report, name):
    return next(d for d in report["detectors"] if d["name"] == name)


def test_tonal_identity_is_clean():
    report = pipeline.run("identity", case=TONAL)
    assert report["verdict"] == "CLEAN"
    assert all(d["fired"] is False for d in report["detectors"])


def test_tonal_grainy_fires_spectral_flux():
    """The whole point: spectral_flux fires on graininess on SUSTAINED material."""
    d = _det(pipeline.run("grainy", case=TONAL), "spectral_flux")
    assert d["fired"] is True, f"spectral_flux missed graininess on tonal: {d['scalar']}"
    assert d["scalar"] >= 0.15


def test_tonal_identity_spectral_flux_quiet():
    d = _det(pipeline.run("identity", case=TONAL), "spectral_flux")
    assert d["fired"] is False and d["scalar"] <= 0.05


def test_tonal_dull_fires_centroid():
    d = _det(pipeline.run("dull", case=TONAL), "spectral_centroid")
    assert d["fired"] is True, f"centroid missed dulling on tonal: {d['scalar']}"


def test_tonal_family_uses_identity_alignment():
    """Proves the generalization: a tonal case runs with IDENTITY alignment — no onset
    detection/map (that's drum-only). The pipeline dispatches on the case's policy."""
    report = pipeline.run("identity", case=TONAL)
    det = report["determinism"]
    assert det["alignment"] == "identity"
    assert "onset_match" not in det  # onset detection is skipped for identity-aligned families
    assert report["case"]["family"] == "tonal"


def test_spectral_flux_quiet_on_drum_smear():
    """Regression / honesty: spectral_flux does NOT false-fire on the percussive smear
    case (transient flux dominates) — which is exactly why it's a tonal-family detector."""
    d = _det(pipeline.run("smear", ["spectral_flux"], case=pipeline.P0A_CASE), "spectral_flux")
    assert d["fired"] is False


def test_drum_family_unbroken():
    """The drum (time-stretch) family still works end to end after the generalization."""
    report = pipeline.run("smear", case=pipeline.P0A_CASE)
    assert _det(report, "transient_sharpness")["fired"] is True
    assert report["determinism"]["alignment"] == "onset-map"
