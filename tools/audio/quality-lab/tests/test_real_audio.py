"""Real-audio input path (reference-free / dry-input): run the real engine on an
arbitrary WAV and check it preserves the source spectrum.

The committed corpus stays synthetic (license-clean); real audio is developer-supplied
(e.g. CC0 files, or local `say` output). This test generates a source in-test rather
than committing real audio. Gated on a built stretchcli.
"""
from __future__ import annotations

import pytest

from quality_lab import audio_io, engine, generate, pipeline

requires_engine = pytest.mark.skipif(not engine.available(), reason="stretchcli not built")


def test_real_audio_skips_without_engine(monkeypatch, tmp_path):
    monkeypatch.setenv(engine.STRETCHCLI_ENV, "/nonexistent/stretchcli-xyz")
    if engine.resolve() is None:
        r = pipeline.run_real_audio(str(tmp_path / "x.wav"))
        assert r["verdict"] == "SKIPPED"


def test_real_audio_missing_input_is_error():
    if engine.available():
        r = pipeline.run_real_audio("/nonexistent/input-xyz.wav")
        assert r["verdict"] == "ERROR"


@requires_engine
def test_real_audio_dry_input_runs_and_is_well_formed(tmp_path):
    """Run the real engine on a WAV via the dry-input path; report is well-formed and
    the clean character preserves the source spectrum (low brightness change)."""
    src = str(tmp_path / "tone.wav")
    y, _ = generate.render_tonal(48000, 2.0, 0)
    audio_io.save_wav(src, y, 48000)

    report = pipeline.run_real_audio(src, ratio=2.0, character="clean")
    assert report["verdict"] in ("CLEAN", "FIRED")
    assert report["case"]["reference_policy"] == "dry-input"
    names = {d["name"] for d in report["detectors"]}
    assert {"spectral_centroid", "hf_fizz", "spectral_flux", "hnr"} == names
    # a faithful clean time-stretch should not wildly alter a steady tone's brightness
    centroid = next(d for d in report["detectors"] if d["name"] == "spectral_centroid")
    assert centroid["scalar"] < 0.5
