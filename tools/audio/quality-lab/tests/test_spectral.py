"""Spectral detector tests: brightness (centroid) deviation + HF fizz.

These are global LTAS metrics (no alignment), so they are robust where onset_drift
was not. Each must fire on its positive control, stay quiet on identity, and not
cross-fire on the other's artifact.
"""
from __future__ import annotations

from quality_lab import pipeline


def _det(report, name):
    return next(d for d in report["detectors"] if d["name"] == name)


def test_centroid_quiet_on_identity():
    d = _det(pipeline.run("identity"), "spectral_centroid")
    assert d["fired"] is False, f"centroid false-fired on identity: {d['scalar']}"


def test_centroid_fires_on_dulling():
    """A whole-signal low-pass (STN-style dulling) must register as a brightness drop."""
    d = _det(pipeline.run("dull"), "spectral_centroid")
    assert d["fired"] is True, f"centroid missed dulling: {d['scalar']}"
    assert "duller" in d["notes"]


def test_hf_fizz_quiet_on_identity():
    d = _det(pipeline.run("identity"), "hf_fizz")
    assert d["fired"] is False, f"hf_fizz false-fired on identity: {d['scalar']}"


def test_hf_fizz_fires_on_added_hf():
    """Added high-frequency noise (metallic sizzle) must fire the fizz detector."""
    d = _det(pipeline.run("fizz"), "hf_fizz")
    assert d["fired"] is True, f"hf_fizz missed added HF: {d['scalar']}"


def test_dulling_does_not_fire_fizz():
    """Removing HF (dulling) must NOT register as added fizz (only added HF fires)."""
    d = _det(pipeline.run("dull"), "hf_fizz")
    assert d["fired"] is False, f"hf_fizz false-fired on dulling: {d['scalar']}"


def test_real_pv_spectral_signature():
    """Document the spectral signature of a REAL phase vocoder (non-circular check):
    it should not crash and should produce finite scalars on a real PV render."""
    from quality_lab import align, audio_io, generate, reference_pv
    from quality_lab.detectors import hf_fizz, spectral_centroid

    sr, ratio = 48000, 1.5
    src, _ = generate.render_drum_break(sr, 120.0, 1.0, 0)
    ref, _ = generate.render_drum_break(sr, 120.0, ratio, 0)
    pv = audio_io.level_match(reference_pv.phase_vocoder_stretch(src, ratio), ref)
    c = spectral_centroid.detect(ref, pv, sr)
    h = hf_fizz.detect(ref, pv, sr)
    assert c.scalar >= 0.0 and h.scalar >= 0.0  # finite, well-formed
