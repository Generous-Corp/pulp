"""Onset-drift detector tests — proves the harness generalizes to a second artifact
class with the same generate -> align -> detect -> report machinery."""
from __future__ import annotations

from quality_lab import pipeline

LOCALIZATION_TOL_S = 0.020


def test_drift_fires_and_localizes():
    """A time-jittered render must fire onset_drift AND localize to a jittered hit."""
    report = pipeline.run("drift", ["onset_drift"])
    det = next(d for d in report["detectors"] if d["name"] == "onset_drift")
    assert det["fired"] is True, f"onset_drift did not fire: {det['scalar']} ms"
    assert det["scalar"] >= 3.0

    _, _, _, injected = pipeline.make_signals("drift")
    worst_t = det["worst_regions"][0]["time_s"]
    nearest = min(abs(worst_t - t) for t in injected)
    assert nearest <= LOCALIZATION_TOL_S, f"{nearest*1000:.1f} ms from nearest jittered onset"


def test_drift_quiet_on_identity():
    """A faithful (identity) render must not register drift."""
    report = pipeline.run("identity", ["onset_drift"])
    det = next(d for d in report["detectors"] if d["name"] == "onset_drift")
    assert det["fired"] is False, f"onset_drift falsely fired: {det['scalar']} ms"
    assert det["scalar"] <= 2.0


def test_drift_quiet_on_smear():
    """Cross-check: an attack-only smear must NOT register as timing drift (the body is
    intact). Proves the two detectors measure orthogonal artifacts."""
    report = pipeline.run("smear", ["onset_drift"])
    det = next(d for d in report["detectors"] if d["name"] == "onset_drift")
    assert det["fired"] is False, f"onset_drift false-positive on smear: {det['scalar']} ms"


def test_transient_detector_quiet_on_drift():
    """Cross-check: a pure timing drift must NOT trip the transient-sharpness detector
    (the attacks are intact, just moved) — detectors measure distinct artifacts."""
    report = pipeline.run("drift", ["transient_sharpness"])
    det = next(d for d in report["detectors"] if d["name"] == "transient_sharpness")
    assert det["fired"] is False, f"transient_sharpness false-positive on drift: {det['scalar']}"


def test_both_detectors_quiet_on_identity():
    """The full P0A detector set stays quiet on a faithful render (negative control)."""
    report = pipeline.run("identity")
    assert report["verdict"] == "CLEAN"
    assert all(d["fired"] is False for d in report["detectors"])
