"""Shared DSP primitives — the pieces with a ground truth known by construction.

Each test here builds an input whose expected answer is a theorem, not a pinned run of the
code, so a regression fails against math rather than against a golden number nobody can
re-derive.
"""
from __future__ import annotations

import numpy as np
import pytest

from quality_lab.dsp import overlapping_allan_deviation


def test_allan_slope_sign_separates_drift_from_jitter():
    """The whole point of Allan deviation over a plain standard deviation: the SIGN of its
    log-log slope tells analog drift from clock jitter, which no single-tau statistic can.

    White frequency noise (independent per-sample jitter) averages DOWN with the averaging
    time as tau^(-1/2) — a NEGATIVE slope. Random-walk frequency noise (drift, the running sum
    of independent kicks) grows as tau^(+1/2) — a POSITIVE slope. Built from exactly those two
    processes — white noise and its cumulative sum — so the expected sign is a theorem, and the
    magnitudes land near the textbook ±1/2.
    """
    rng = np.random.default_rng(0)
    n = 4096
    jitter_series = rng.standard_normal(n)                 # white FM  → clock jitter
    drift_series = np.cumsum(rng.standard_normal(n))       # random-walk FM → analog drift

    jitter = overlapping_allan_deviation(jitter_series)
    drift = overlapping_allan_deviation(drift_series)

    assert jitter.slope < 0.0 < drift.slope, (
        f"the slope sign must separate the two noise types: "
        f"jitter={jitter.slope:.3f}, drift={drift.slope:.3f}"
    )
    assert jitter.slope == pytest.approx(-0.5, abs=0.2), f"white FM slope off tau^(-1/2): {jitter.slope:.3f}"
    assert drift.slope == pytest.approx(0.5, abs=0.25), f"random-walk FM slope off tau^(+1/2): {drift.slope:.3f}"


def test_allan_reports_octave_spaced_taus_over_the_record():
    """The estimator must actually sweep a range of averaging times — a single tau cannot show
    a slope. Octave spacing up to ~half the record gives ~log2(N) points, all positive."""
    dev = overlapping_allan_deviation(np.random.default_rng(1).standard_normal(1024))
    assert dev.taus.size >= 4 and np.all(dev.deviations > 0.0)
    assert np.array_equal(dev.taus, np.asarray([1 << k for k in range(dev.taus.size)], dtype=float)), (
        "averaging factors must be octave-spaced 1, 2, 4, …"
    )


def test_allan_is_flat_for_a_too_short_series():
    """Degenerate guard: a series too short to form two averaging factors cannot define a
    slope, so it returns a 0.0 slope rather than raising or extrapolating from one point."""
    for series in (np.array([]), np.array([1.0]), np.array([1.0, 2.0, 3.0])):
        r = overlapping_allan_deviation(series)
        assert r.slope == 0.0
        assert r.taus.size == r.deviations.size
        assert r.taus.size < 2
