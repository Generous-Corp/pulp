"""P0a go/no-go gate (§5 P0a, acceptance #1/#4).

The whole proposal hinges on this: with alignment + one detector, can the lab
LOCALIZE a known transient smear (within +/-20 ms) AND stay QUIET on an identity
render? If not, the architecture is wrong and we stop before building the corpus.
"""
from __future__ import annotations

import numpy as np

from quality_lab import pipeline
from quality_lab.detectors import transient_sharpness

LOCALIZATION_TOL_S = 0.020  # the +/-20 ms acceptance bar
QUIET_TOL = 0.05  # identity must read essentially perfect
FIRE_MIN = 0.20  # a real smear must clearly fire


def _injected_onsets(latency_ms=5.0):
    _, _, _, injected = pipeline.make_p0a_signals(smear=True, latency_ms=latency_ms)
    return injected


def test_positive_fires_and_localizes():
    """A deliberately smeared candidate must fire AND its worst region must land
    within +/-20 ms of an injected onset."""
    report = pipeline.run_p0a(smear=True, latency_ms=5.0, smear_ms=8.0)
    det = report["detectors"][0]
    assert det["name"] == "transient_sharpness"
    assert det["fired"] is True, f"detector did not fire: scalar={det['scalar']}"
    assert det["scalar"] >= FIRE_MIN

    injected = _injected_onsets(latency_ms=5.0)
    assert report["worst_regions"], "no worst region reported"
    worst_t = report["worst_regions"][0]["time_s"]
    nearest = min(abs(worst_t - t) for t in injected)
    assert nearest <= LOCALIZATION_TOL_S, (
        f"worst region t={worst_t:.3f}s is {nearest * 1000:.1f} ms from the nearest "
        f"injected onset (tol {LOCALIZATION_TOL_S * 1000:.0f} ms)"
    )


def test_negative_stays_quiet():
    """An identity (no-op) render must stay quiet on the detector."""
    report = pipeline.run_p0a(smear=False, latency_ms=5.0)
    det = report["detectors"][0]
    assert det["fired"] is False, f"detector falsely fired on identity: {det['scalar']}"
    assert det["scalar"] <= QUIET_TOL, f"identity not quiet: scalar={det['scalar']}"


def test_alignment_matches_all_onsets():
    """Onset-map alignment must pair every reference onset despite candidate latency
    (proves alignment is real, not a no-op)."""
    from quality_lab import align

    ref, cand, sr, _ = pipeline.make_p0a_signals(smear=False, latency_ms=5.0)
    cand = align.detect_onsets(cand, sr)
    ref_on = align.detect_onsets(ref, sr)
    pairs = align.map_onsets(ref_on, cand, len(ref) / sr, len(ref) / sr + 0.005)
    assert len(pairs) == len(ref_on) and len(ref_on) > 4


def test_identity_attack_slope_is_equal():
    """Sanity: the same attack measured at a quantization-shifted onset gives a near-
    identical slope (the refinement invariant that makes the negative control quiet)."""
    from quality_lab import align

    ref, _cand, sr, _ = pipeline.make_p0a_signals(smear=False, latency_ms=0.0)
    onset = align.detect_onsets(ref, sr)[0]
    s0 = transient_sharpness._attack_slope(ref, sr, onset)
    s_shift = transient_sharpness._attack_slope(ref, sr, onset + 0.003)  # +3 ms jitter
    assert s0 > 0.0
    assert abs(s0 - s_shift) / s0 < 0.05, "attack-slope not robust to onset jitter"
