"""Tier 1 — fixed-latency-trim alignment for compare (`--align latency`).

A constant delay/offset used to make the phase-sensitive null-residual false-alarm as "material"
even when nothing tonal changed. `--align latency` estimates a single constant lag, trims to a
common time base, and measures the aligned pair — so a pure delay reads as the identity it is
(corroborated), while a real change under a delay stays visible. Refuses (records `not_aligned`)
when the difference is not a reliable pure delay: a wrong alignment is worse than none. Default
`none` preserves every existing contract.
"""
from __future__ import annotations

import numpy as np

from quality_lab import audio_io, compare, generate
from quality_lab.dsp import apply_lag_trim, estimate_global_lag


def _drum(seed: int = 0):
    y, _ = generate.render_drum_break(sr=48000, seed=seed)
    return y, 48000


def _delay(y, n):
    """A realistic constant delay: shift `y` later by n samples (front zero-fill), tail PRESERVED
    (a delay plugin lengthens the render, it doesn't drop the end). apply_lag_trim removes the lead
    only, so a pure delay recovers to identity while any real added tail stays visible."""
    return np.concatenate([np.zeros(n), y])


# ── dsp primitives ───────────────────────────────────────────────────────────────────────

def test_estimate_global_lag_is_exact_on_a_pure_delay():
    ref, sr = _drum()
    for n in (48, 240, 1000):
        est = estimate_global_lag(ref, _delay(ref, n), sr)
        assert est.lag_samples == n and est.confidence > 0.9


def test_estimate_global_lag_low_confidence_on_unrelated_signals():
    ref, sr = _drum()
    other, _ = _drum(seed=7)
    assert estimate_global_lag(ref, other, sr).confidence < 0.7     # not a pure delay → refuse


def test_estimate_global_lag_refuses_periodic_material():
    """A periodic/tonal signal has a COMB of equal correlation peaks, so no single lag is the true
    one — the estimator must return confidence 0 (refuse), not lock onto a spurious half-period lag."""
    sr = 48000
    t = np.arange(sr) / sr
    sine = 0.3 * np.sin(2 * np.pi * 1000.0 * t)
    assert estimate_global_lag(sine, _delay(sine, 240), sr).confidence == 0.0


def test_estimate_global_lag_refuses_out_of_range_delay():
    """A delay beyond the ±max_lag search window can only produce a spurious in-window peak — the
    boundary guard must refuse it rather than confidently trim a wrong lag."""
    ref, sr = _drum()
    est = estimate_global_lag(ref, _delay(ref, 30000), sr)          # 30k samples > 0.5 s @ 48k
    assert est.confidence == 0.0


def test_apply_lag_trim_recovers_identity_on_a_pure_delay():
    ref, sr = _drum()
    d = _delay(ref, 240)
    ra, ca = apply_lag_trim(ref, d, 240)
    assert np.allclose(ra, ca, atol=1e-9)                           # aligned content is identical


# ── compare `--align latency` ────────────────────────────────────────────────────────────

def test_pure_delay_reads_not_corroborated_without_align():
    """Default (no alignment): tonal-balance sees no tonal change (centroid is delay-invariant), but
    the sample residual DOES see the shift → not_corroborated + an uncaptured-difference flag."""
    ref, sr = _drum()
    report = compare.compare_arrays(ref, _delay(ref, 240), sr, reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE
    assert report["advisory"]["corroboration"]["status"] == compare.NOT_CORROBORATED
    assert compare.FLAG_UNCAPTURED_DIFF in [f["flag"] for f in report["headline_flags"]]


def test_pure_delay_is_corroborated_with_align_latency():
    """With `--align latency` the constant lag is trimmed, so the residual reads identity and the
    delay no longer masquerades as an uncaptured material difference."""
    ref, sr = _drum()
    report = compare.compare_arrays(ref, _delay(ref, 240), sr, reference_role="golden", align="latency")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE
    m = report["measurements"][0]
    assert m["alignment"]["policy"] == "fixed-latency-trim"
    assert m["alignment"]["lag_samples"] == 240 and m["alignment"]["applied"] is True
    assert report["advisory"]["corroboration"]["status"] == compare.CORROBORATED
    assert report["headline_flags"] == []
    assert "Aligned" in report["summary"]


def test_real_change_under_a_delay_is_still_caught_after_align():
    """A genuine tonal change hidden behind a delay: aligning removes the shift and the change
    (a dulling) is measured + corroborated — the point of aligning."""
    ref, sr = _drum()
    cand = generate.dull(_delay(ref, 240), sr)
    report = compare.compare_arrays(ref, cand, sr, reference_role="golden", align="latency")
    assert report["verdict"] == compare.VERDICT_REGRESSION           # the dulling is caught
    assert report["measurements"][0]["alignment"]["policy"] == "fixed-latency-trim"
    assert report["advisory"]["corroboration"]["status"] == compare.CORROBORATED


def test_align_refuses_on_low_confidence_and_measures_unaligned():
    """When the difference is not a reliable constant lag (two unrelated renders), alignment must
    REFUSE — record `not_aligned`, do not trim, and fall through to unaligned measurement."""
    ref, sr = _drum()
    other, _ = _drum(seed=7)
    report = compare.compare_arrays(ref, other, sr, reference_role="golden", align="latency")
    al = report["measurements"][0]["alignment"]
    assert al["policy"] == "not_aligned" and al["applied"] is False
    assert "refused" in report["summary"].lower()


def test_align_none_leaves_the_default_alignment_policy():
    ref, sr = _drum()
    report = compare.compare_arrays(ref, ref.copy(), sr, align="none")
    assert report["measurements"][0]["alignment"] == {"policy": "not_required", "reason": "global_ltas_metric"}


def test_unknown_align_raises():
    ref, sr = _drum()
    try:
        compare.compare_arrays(ref, ref.copy(), sr, align="warp")
    except ValueError:
        return
    raise AssertionError("unknown align should raise ValueError")


def test_align_does_not_move_the_verdict_independently():
    """The aligned path still routes the verdict purely through the primary envelope + role."""
    ref, sr = _drum()
    report = compare.compare_arrays(ref, _delay(ref, 240), sr, reference_role="golden", align="latency")
    axis = compare._resolve(report["profile"])
    assert report["verdict"] == compare._verdict(axis, report["measurements"][0], "golden")


def test_align_does_not_hide_added_tail_content():
    """A candidate that is a delayed reference PLUS extra tail content must NOT read as identity —
    apply_lag_trim removes only the lead, so the residual (which zero-pads the tail) still flags the
    added tail. Trimming must never truncate a real difference into a false clean verdict."""
    ref, sr = _drum()
    tail, _ = _drum(seed=3)
    cand = np.concatenate([np.zeros(240), ref, 0.5 * tail])          # delayed ref + a loud extra tail
    report = compare.compare_arrays(ref, cand, sr, reference_role="golden", align="latency")
    assert report["measurements"][0]["alignment"]["policy"] == "fixed-latency-trim"
    # the tail is a real difference — the residual must register it (NOT a -160 dB identity)
    assert report["advisory"]["corroboration"]["basis"]["raw_material"] is True


def test_alignment_records_are_independent_dicts():
    """The alignment record must be a FRESH dict per report — mutating one must not corrupt another
    (a real hazard in the persistent MCP process where reports share a module singleton)."""
    ref, sr = _drum()
    r1 = compare.compare_arrays(ref, ref.copy(), sr, align="none")
    r2 = compare.compare_arrays(ref, ref.copy(), sr, align="none")
    r1["measurements"][0]["alignment"]["policy"] = "MUTATED"
    assert r2["measurements"][0]["alignment"]["policy"] == "not_required"


def test_stereo_width_align_latency_discloses_the_skip(tmp_path):
    """`--align latency` on stereo-width is a no-op (the metric is delay-invariant), but the record
    must disclose that latency was requested and skipped, not a stale 'not requested'."""
    from quality_lab import generate as gen
    st = gen.render_stereo_pad(sr=48000)
    golden, cand = str(tmp_path / "g.wav"), str(tmp_path / "c.wav")
    audio_io.save_wav(golden, st, 48000)
    audio_io.save_wav(cand, gen.narrow_stereo(st, amount=1.0), 48000)
    report = compare.compare_files(golden, cand, profile="stereo-width",
                                   reference_role="golden", align="latency")
    al = report["measurements"][0]["alignment"]
    assert al["policy"] == "not_required" and al.get("requested") == "latency"


def test_cli_align_latency_smoke(tmp_path, capsys):
    from quality_lab import cli
    ref, sr = _drum()
    ref_p, cand_p = str(tmp_path / "r.wav"), str(tmp_path / "c.wav")
    audio_io.save_wav(ref_p, ref, sr)
    audio_io.save_wav(cand_p, _delay(ref, 240), sr)
    rc = cli.main(["compare", ref_p, cand_p, "--reference-role", "golden", "--align", "latency"])
    assert rc == 0
    assert "Aligned" in capsys.readouterr().out
