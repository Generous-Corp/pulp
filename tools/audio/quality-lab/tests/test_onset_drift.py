"""onset_drift validation — the calibration sweep the original deferral FAILED.

The detector measures the event-time residual after removing the common latency. We prove:
(1) it recovers an injected linear drift within the analyzer floor (a linear ramp
    0->D gives max|residual| = D/2 once the median offset is removed),
(2) a pure uniform latency reads ~0 drift (it is reported as common_latency, not a defect),
(3) identity reads ~0 and does not fire,
(4) drift beyond the search window drops to low coverage (refuses to invent a number),
(5) it ships experimental and cannot move the verdict through the pipeline.
"""
from __future__ import annotations

import numpy as np

from quality_lab import generate, pipeline
from quality_lab.detectors import onset_drift

SR = 48000


def _drum():
    ref, onsets = generate.render_drum_break(SR, 120.0, 1.5, 0)
    pairs = [(o, o) for o in onsets]  # ref_t is all onset_drift needs; cand_t unused
    return ref, onsets, pairs


def test_uniform_latency_is_not_drift():
    """A constant delay on every onset is benign latency, not groove damage."""
    ref, onsets, pairs = _drum()
    cand = generate.drift(ref, onsets, SR, lag_ms_start=6.0, lag_ms_end=6.0)
    d = onset_drift.detect(ref, cand, SR, pairs)
    assert d.scalar < 1.0, f"uniform latency leaked into drift scalar: {d.scalar}"
    assert not d.fired
    assert abs(float(d.notes.split("common_latency=")[1].split("ms")[0]) - 6.0) < 1.5


def test_identity_is_quiet():
    ref, onsets, pairs = _drum()
    d = onset_drift.detect(ref, ref, SR, pairs)
    assert d.scalar < 1.0 and not d.fired


def test_calibration_sweep_recovers_injected_drift():
    """A linear ramp 0->D (max per-onset lag = D) should be recovered as max|residual|
    ~= D/2 once the median latency is removed — the test the body-correlation version
    could not pass. Accuracy holds only IN RANGE (max lag < the ~12 ms search window);
    beyond that the analyzer honestly drops onsets (covered separately)."""
    ref, onsets, pairs = _drum()
    # In-range drifts only (peak lag <= 8 ms, comfortably inside the 12 ms search window).
    measured = {}
    for D in (0.0, 2.0, 4.0, 8.0):
        cand = generate.drift(ref, onsets, SR, lag_ms_start=0.0, lag_ms_end=D)
        measured[D] = onset_drift.detect(ref, cand, SR, pairs).scalar
    vals = [measured[D] for D in (0.0, 2.0, 4.0, 8.0)]
    assert all(b >= a - 0.3 for a, b in zip(vals, vals[1:])), f"not monotonic: {vals}"
    # recovers D/2 within 1 ms — far under the 2.67 ms onset hop → fine cross-correlation
    # genuinely beats hop quantization (the proof the plan demands).
    for D in (4.0, 8.0):
        assert abs(measured[D] - D / 2.0) <= 1.0, f"D={D}: recovered {measured[D]}, want ~{D/2}"
    assert measured[0.0] < 1.0  # identity floor


def test_drift_beyond_search_window_reads_low_coverage():
    """A drift larger than the cross-correlation search window must drop onsets, not
    report a confident-but-wrong number (H3 coverage honesty)."""
    ref, onsets, pairs = _drum()
    cand = generate.drift(ref, onsets, SR, lag_ms_start=0.0, lag_ms_end=40.0)  # > 12ms search
    d = onset_drift.detect(ref, cand, SR, pairs)
    assert d.measured < d.expected, "should have dropped saturated onsets"


def test_fires_on_a_real_drift():
    ref, onsets, pairs = _drum()
    cand = generate.drift(ref, onsets, SR, lag_ms_start=0.0, lag_ms_end=10.0)
    d = onset_drift.detect(ref, cand, SR, pairs)
    assert d.fired and d.scalar >= 1.5
    assert d.maturity == "experimental"


# ── pipeline: experimental → advisory, cannot move the verdict ────────────

def test_onset_drift_is_advisory_in_the_pipeline():
    """onset_drift runs on the percussive case but, being experimental, never moves the
    headline verdict — a clean (identity) run stays CLEAN even though onset_drift is on."""
    report = pipeline.run("identity", case=pipeline.P0A_CASE)
    names = [d["name"] for d in report["detectors"]]
    assert "onset_drift" in names
    det = next(d for d in report["detectors"] if d["name"] == "onset_drift")
    assert det["maturity"] == "experimental"
    assert det["participates_in_verdict"] is False
    assert report["verdict"] == "CLEAN"
