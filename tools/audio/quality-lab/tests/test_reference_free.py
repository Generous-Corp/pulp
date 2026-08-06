from __future__ import annotations

import json
import hashlib

import numpy as np

from quality_lab import audio_io, reference_free


def _am_tone(sr: int, rate_hz: float, seconds: float = 4.0) -> np.ndarray:
    t = np.arange(int(sr * seconds), dtype=np.float64) / sr
    envelope = 0.55 + 0.45 * np.sin(2.0 * np.pi * rate_hz * t)
    return envelope * np.sin(2.0 * np.pi * 220.0 * t)


def _digest(path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _manifest(tmp_path, paths, holdout=True):
    def split(name, pair_id, seed):
        return {"pair_id": pair_id, "prompt": "make me a cello",
                "inventory_sha256": "a" * 64, "model": "fixture-model",
                "audibility_gate": {"analysis": "audible-band-rms-v1",
                                     "minimum_rms": 1e-4},
                "arms": {
                    "without": {"guidance": "off", "attempt": 1, "seed": seed,
                                "generation_id": f"{pair_id}-off",
                                "wav_sha256": _digest(tmp_path / f"{name}off.wav")},
                    "with": {"guidance": "on", "attempt": 1, "seed": seed,
                             "generation_id": f"{pair_id}-on",
                             "wav_sha256": _digest(tmp_path / f"{name}on.wav")}}}
    doc = {"schema": "quality_lab.catalogue_experiment.v1",
           "working": split("", "working-1", 101)}
    if holdout:
        doc["holdout"] = split("holdout-", "holdout-1", 202)
    path = tmp_path / "experiment.json"
    path.write_text(json.dumps(doc))
    return str(path)


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
                       ("holdout-off", 5.2), ("holdout-on", 7.45)):
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
        paths["off"], paths["on"], str(expect), _manifest(tmp_path, paths),
        paths["holdout-off"], paths["holdout-on"])
    assert report["verdict"] == "CATALOGUE_IMPROVES"
    assert report["goodhart_guard"]["accepted"] is True
    assert "held-out" in report["goodhart_guard"]["reason"]
    assert report["holdout"]["scores"]["with"]["scores"][
        "amplitude_modulation_hz"] < 1.0


def test_experiment_requires_matched_arms_and_independent_audio(tmp_path):
    import pytest
    paths = {}
    for name, rate in (("off", 5.0), ("on", 7.5),
                       ("holdout-off", 5.2), ("holdout-on", 7.45)):
        path = tmp_path / f"{name}.wav"
        audio_io.save_wav(str(path), _am_tone(48000, rate), 48000)
        paths[name] = str(path)

    manifest = _manifest(tmp_path, paths)
    doc = json.loads((tmp_path / "experiment.json").read_text())
    doc["working"]["arms"]["with"]["attempt"] = 2
    (tmp_path / "experiment.json").write_text(json.dumps(doc))
    with pytest.raises(ValueError, match="same attempt"):
        reference_free._experiment(manifest, {
            "working.without": paths["off"], "working.with": paths["on"],
            "holdout.without": paths["holdout-off"],
            "holdout.with": paths["holdout-on"]}, True)

    # A different container body around exactly the same decoded samples is
    # not an independent holdout generation.
    duplicate = tmp_path / "holdout-on.wav"
    duplicate.write_bytes((tmp_path / "on.wav").read_bytes() + b"ignored-trailer")
    _manifest(tmp_path, paths)
    with pytest.raises(ValueError, match="decoded audio"):
        reference_free._experiment(manifest, {
            "working.without": paths["off"], "working.with": paths["on"],
            "holdout.without": paths["holdout-off"],
            "holdout.with": paths["holdout-on"]}, True)


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
        reference_free.compare_files(str(path), str(path), str(expect),
                                     str(tmp_path / "missing-experiment.json"))

    expect.write_text(json.dumps({"source": {"path": "book.pdf", "page": None},
                                  "metrics": {
        "amplitude_modulation_hz": {"target": 7.5, "tolerance": 0.5}}}))
    with pytest.raises(ValueError, match="positive source.page"):
        reference_free.compare_files(str(path), str(path), str(expect),
                                     str(tmp_path / "missing-experiment.json"))

    for invalid_path in ("   ", [], {}):
        expect.write_text(json.dumps({
            "source": {"path": invalid_path, "page": 1},
            "metrics": {"amplitude_modulation_hz": {
                "target": 7.5, "tolerance": 0.5}}}))
        with pytest.raises(ValueError, match="source.path"):
            reference_free.compare_files(
                str(path), str(path), str(expect),
                str(tmp_path / "missing-experiment.json"))


def test_catalogue_ab_refuses_one_target_regression():
    base = {"metrics": {"a": 10.0, "b": 10.0}, "confidence": {},
            "audibility": {"rms": 1.0, "audible_band_rms": 1.0}}
    candidate = {"metrics": {"a": 9.0, "b": 13.0}, "confidence": {},
                 "audibility": {"rms": 1.0, "audible_band_rms": 1.0}}
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


def test_silence_cannot_win_a_max_only_expectation():
    report = reference_free.analyze_samples(np.zeros(48000), 48000)
    import pytest
    with pytest.raises(ValueError, match="silent"):
        reference_free.score(report, {"metrics": {
            "spectral_flux": {"max": 1.0, "tolerance": 0.1}}}, "silent")


def test_dc_and_ultra_quiet_audio_cannot_win():
    import pytest
    for samples in (np.full(48000, 2e-4),
                    _am_tone(48000, 7.5) * 1e-5):
        report = reference_free.analyze_samples(samples, 48000)
        with pytest.raises(ValueError, match="silent"):
            reference_free.score(report, {"metrics": {
                "spectral_flux": {"max": 1.0, "tolerance": 0.1}}}, "inaudible")


def test_multichannel_catalogue_input_uses_disclosed_loudest_channel(tmp_path):
    path = tmp_path / "stereo.wav"
    tone = _am_tone(48000, 7.5)
    audio_io.save_wav(str(path), np.stack([tone, -tone], axis=1), 48000)
    report = reference_free.analyze_file(str(path))
    assert report["channel_handling"] == {
        "input_channels": 2,
        "analysis": "loudest-listener-channel",
        "selected_channel": 0,
    }
    assert report["audibility"]["audible_band_rms"] > 0.1


def test_hf_metric_is_absent_when_the_band_is_degenerate():
    report = reference_free.analyze_samples(_am_tone(16000, 7.5), 16000)
    assert "hf_energy_fraction" not in report["metrics"]


def test_low_confidence_holdout_cannot_promote():
    from quality_lab import loop
    champ = loop.CandidateScore("off", {"x": 2.0}, 1.0)
    cand = loop.CandidateScore("on", {"x": 0.0}, 1.0)
    ho_champ = loop.CandidateScore("ho-off", {"x": 2.0}, 1.0)
    ho_cand = loop.CandidateScore("ho-on", {"x": 0.0}, 0.01)
    verdict = loop.goodhart_guard(cand, champ, ho_cand, ho_champ)
    assert verdict["accepted"] is False
    assert "held-out" in verdict["reason"] and verdict["needs_ear"] is True


def test_low_confidence_baselines_cannot_promote():
    from quality_lab import loop
    champ = loop.CandidateScore("off", {"x": 2.0}, 0.01)
    cand = loop.CandidateScore("on", {"x": 0.0}, 1.0)
    ho_champ = loop.CandidateScore("ho-off", {"x": 2.0}, 0.01)
    ho_cand = loop.CandidateScore("ho-on", {"x": 0.0}, 1.0)
    verdict = loop.goodhart_guard(cand, champ, ho_cand, ho_champ)
    assert verdict["accepted"] is False
    assert "baseline confidence" in verdict["reason"]
