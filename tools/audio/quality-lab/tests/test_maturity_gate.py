"""The maturity gate — experimental detectors are advisory and cannot move the verdict
or the regression gate; promotion to beta is the only thing that lets a detector count.

This is the safety foundation for shipping unproven detectors (onset_drift, etc.): a
brand-new detector can run and report on every case while being *structurally incapable*
of changing any existing pass/fail.
"""
from __future__ import annotations

from dataclasses import replace

from quality_lab import pipeline
from quality_lab.schema import (
    DetectorResult,
    WorstRegion,
    detectors_for_engine_baseline,
    detectors_for_verdict,
)


def _fake(name, *, fired, maturity):
    """A detector fn that always returns the same result, with a worst region so we can
    prove experimental regions never surface as a headline."""
    def detect(reference, candidate, sr, onset_pairs=None):
        return DetectorResult(
            name=name, scalar=99.0, unit="x", fired=fired, time_domain="aligned",
            measured=1, expected=1, maturity=maturity,
            worst_regions=[WorstRegion(time_s=0.1, severity=9.0, detector=name)],
        )
    return detect


def _case_with(tag):
    return replace(pipeline.TONAL_CASE,
                   detector_tags=[*pipeline.TONAL_CASE.detector_tags, tag])


# ── policy helpers ────────────────────────────────────────────────────────

def test_policy_helpers_partition_by_maturity():
    res = [
        DetectorResult("s", 0, "x", False, "aligned", maturity="stable"),
        DetectorResult("b", 0, "x", True, "aligned", maturity="beta"),
        DetectorResult("e", 0, "x", True, "aligned", maturity="experimental"),
    ]
    assert [d.name for d in detectors_for_verdict(res)] == ["s", "b"]      # exp excluded
    assert [d.name for d in detectors_for_engine_baseline(res)] == ["s"]   # only stable


# ── H1: always-firing experimental cannot move the headline ───────────────

def test_always_firing_experimental_does_not_move_verdict(monkeypatch):
    base = pipeline.run("identity", case=pipeline.TONAL_CASE)
    assert base["verdict"] == "CLEAN" and base["worst_regions"] == []

    monkeypatch.setitem(pipeline._DETECTORS, "always_fire_exp",
                        _fake("always_fire_exp", fired=True, maturity="experimental"))
    withexp = pipeline.run("identity", case=_case_with("always_fire_exp"))

    # Headline verdict + top-level worst_regions are byte-identical to the run without it.
    assert withexp["verdict"] == base["verdict"]
    assert withexp["worst_regions"] == base["worst_regions"]

    # It IS present and visible — flagged advisory, never gate-participating.
    det = next(d for d in withexp["detectors"] if d["name"] == "always_fire_exp")
    assert det["fired"] is True
    assert det["participates_in_verdict"] is False
    assert det["participates_in_engine_baseline"] is False
    assert det["maturity"] == "experimental"
    # ...and surfaced in the advisory namespace.
    assert any(d["name"] == "always_fire_exp" for d in withexp["advisory"]["detectors"])


def test_promotion_to_beta_flips_the_verdict(monkeypatch):
    """The ONLY thing that lets the same firing detector count is promotion to beta."""
    monkeypatch.setitem(pipeline._DETECTORS, "fire_beta",
                        _fake("fire_beta", fired=True, maturity="beta"))
    r = pipeline.run("identity", case=_case_with("fire_beta"))
    assert r["verdict"] == "FIRED"  # beta participates in the verdict
    # ...but is still held out of the regression baseline by default.
    det = next(d for d in r["detectors"] if d["name"] == "fire_beta")
    assert det["participates_in_verdict"] is True
    assert det["participates_in_engine_baseline"] is False


def test_engine_baseline_capture_excludes_non_stable():
    """The capture comprehension keys off the JSON flag — prove the flag gates it."""
    report_detectors = [
        {"name": "stable_d", "scalar": 1.0, "participates_in_engine_baseline": True},
        {"name": "beta_d", "scalar": 2.0, "participates_in_engine_baseline": False},
        {"name": "exp_d", "scalar": 3.0, "participates_in_engine_baseline": False},
    ]
    captured = {d["name"]: d["scalar"] for d in report_detectors
                if d.get("participates_in_engine_baseline", True)}
    assert captured == {"stable_d": 1.0}


# ── back-compat ───────────────────────────────────────────────────────────

def test_existing_detectors_default_stable_and_participate():
    r = pipeline.run("smear", case=pipeline.P0A_CASE)
    for d in r["detectors"]:
        assert d["maturity"] == "stable"
        assert d["participates_in_verdict"] is True
