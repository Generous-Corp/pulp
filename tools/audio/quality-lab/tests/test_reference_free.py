from __future__ import annotations

import json

import numpy as np

from quality_lab import audio_io, reference_free


def _am_tone(sr: int, rate_hz: float, seconds: float = 4.0) -> np.ndarray:
    t = np.arange(int(sr * seconds), dtype=np.float64) / sr
    envelope = 0.55 + 0.45 * np.sin(2.0 * np.pi * rate_hz * t)
    return envelope * np.sin(2.0 * np.pi * 220.0 * t)


def test_single_render_reports_catalogue_relevant_gate_fields():
    report = reference_free.analyze_samples(_am_tone(48000, 7.5), 48000)
    assert report["schema"] == "quality_lab.reference_free.v1"
    assert abs(report["metrics"]["amplitude_modulation_hz"] - 7.5) < 0.3
    assert report["confidence"]["amplitude_modulation_hz"] > 0.5
    assert set(report["metrics"]) == {
        "spectral_centroid_hz", "hf_energy_fraction", "spectral_flux",
        "hnr_db", "amplitude_modulation_hz",
    }


def test_catalogue_ab_requires_pareto_target_improvement_and_holdout(tmp_path):
    sr = 48000
    paths = {}
    for name, rate in (("off", 5.0), ("on", 7.5),
                       ("holdout-off", 5.2), ("holdout-on", 7.4)):
        path = tmp_path / f"{name}.wav"
        audio_io.save_wav(str(path), _am_tone(sr, rate), sr)
        paths[name] = str(path)
    expect = tmp_path / "expect.json"
    expect.write_text(json.dumps({"schema_version": 1,
                                  "source": {"path": "Welsh.pdf", "page": 42},
                                  "metrics": {
        "amplitude_modulation_hz": {"target": 7.5, "tolerance": 0.5}
    }}))

    report = reference_free.compare_files(
        paths["off"], paths["on"], str(expect),
        paths["holdout-off"], paths["holdout-on"])
    assert report["verdict"] == "CATALOGUE_IMPROVES"
    assert report["goodhart_guard"]["accepted"] is True
    assert "held-out" in report["goodhart_guard"]["reason"]
    assert report["holdout"]["scores"]["with"]["scores"][
        "amplitude_modulation_hz"] < 1.0


def test_catalogue_ab_needs_a_cited_source(tmp_path):
    sr = 48000
    path = tmp_path / "tone.wav"
    audio_io.save_wav(str(path), _am_tone(sr, 7.5), sr)
    expect = tmp_path / "expect.json"
    expect.write_text(json.dumps({"metrics": {
        "amplitude_modulation_hz": {"target": 7.5, "tolerance": 0.5}
    }}))
    import pytest
    with pytest.raises(ValueError, match="source.path"):
        reference_free.compare_files(str(path), str(path), str(expect))


def test_catalogue_ab_refuses_one_target_regression():
    base = {"metrics": {"a": 10.0, "b": 10.0}, "confidence": {}}
    candidate = {"metrics": {"a": 9.0, "b": 13.0}, "confidence": {}}
    expectations = {"metrics": {
        "a": {"target": 8.0, "tolerance": 1.0},
        "b": {"target": 10.0, "tolerance": 1.0},
    }}
    off = reference_free.score(base, expectations, "off")
    on = reference_free.score(candidate, expectations, "on")
    from quality_lab import loop
    verdict = loop.goodhart_guard(on, off, thresholds={"a": 1.0, "b": 1.0})
    assert verdict["accepted"] is False
    assert "Pareto" in verdict["reason"]
