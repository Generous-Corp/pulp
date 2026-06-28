"""Real-engine regression gate: the lab catches when an engine change deviates from the
committed baseline. The COMPARISON logic is unit-tested without the engine (runs in CI);
the live check against the real engine is gated on a built stretchcli."""
from __future__ import annotations

import pytest

from quality_lab import engine, engine_baseline

requires_engine = pytest.mark.skipif(
    not engine.available(), reason="stretchcli not built"
)


def test_compare_clean_when_matching():
    m = {"r=2,clean": {"transient_sharpness": 1.0, "spectral_centroid": 0.02}}
    assert engine_baseline.compare(m, m) == []


def test_compare_flags_a_worse_transient_regression():
    base = {"r=2,clean": {"transient_sharpness": 0.5}}
    worse = {"r=2,clean": {"transient_sharpness": 1.0}}  # more smear = worse
    dev = engine_baseline.compare(base, worse)
    assert len(dev) == 1 and dev[0]["worse"] is True and dev[0]["delta"] == 0.5


def test_compare_flags_any_centroid_drift():
    base = {"r=2,vari": {"spectral_centroid": 0.30}}
    drifted = {"r=2,vari": {"spectral_centroid": 0.45}}
    dev = engine_baseline.compare(base, drifted)
    assert len(dev) == 1 and dev[0]["worse"] is False  # drift surfaced, not labeled worse


def test_compare_within_tolerance_is_clean():
    base = {"k": {"spectral_centroid": 0.300}}
    cur = {"k": {"spectral_centroid": 0.315}}  # within 0.03 tolerance
    assert engine_baseline.compare(base, cur) == []


def test_committed_baseline_is_well_formed():
    b = engine_baseline.load_baseline()
    assert b["schema_version"] == 1 and b["matrix"]
    # every matrix entry has the drum-family detectors
    for dets in b["matrix"].values():
        assert "transient_sharpness" in dets


@requires_engine
def test_current_engine_matches_committed_baseline():
    """When stretchcli is built, the current engine must match its committed baseline
    (deterministic) — proving the regression gate is anchored to real engine behavior."""
    assert engine_baseline.check() == []
