"""Slice 3 — corroboration promoted into the headline via structured `headline_flags`.

When the deterministic null-residual DISAGREES with the primary axis, that is often the most
actionable line for a DSP dev (a real change the axis can't see, or an axis flag the residual
won't back). It used to sit buried in `advisory.corroboration`; these tests pin that it is now
promoted — as a machine-readable top-level flag AND a summary sentence — in BOTH directions,
without ever moving the verdict, and that the known time-variant false-alarm class is
machine-suppressible via `expected_for`.
"""
from __future__ import annotations

import numpy as np

from quality_lab import cli, compare, generate
from quality_lab.dsp import highband


def _drum(seed: int = 0):
    y, _ = generate.render_drum_break(sr=48000, seed=seed)
    return y, 48000


def test_uncaptured_material_difference_flag_when_axis_is_blind():
    """A pure delay: the tonal axis sees no change but the sample-domain residual is material — the
    'a real difference this axis can't see' signal. It must surface as a top-level flag AND a
    summary sentence, and (being the time-variant false-alarm class) carry `expected_for`."""
    ref, sr = _drum()
    cand = np.roll(ref, 64)                                   # whole-signal shift: tone same, phase differs
    report = compare.compare_arrays(ref, cand, sr, profile="tonal-balance", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE    # axis unchanged
    flags = report["headline_flags"]
    assert len(flags) == 1 and flags[0]["flag"] == compare.FLAG_UNCAPTURED_DIFF
    assert "time_variant_processing" in flags[0]["expected_for"]   # machine-suppressible (F2')
    assert "does not measure" in report["summary"]


def test_axis_change_without_residual_flag():
    """The mirror direction: the axis flags a change (tiny centroid shift under a tiny threshold)
    that the sample-domain residual does not register (near identity) — a marginal/phase-only
    change to weigh before acting. Must surface as the mirror flag + summary caution."""
    ref, sr = _drum()
    cand = ref + 3e-4 * highband(ref)                        # tiny HF nudge: axis just exceeds, residual ~-70 dB
    report = compare.compare_arrays(ref, cand, sr, profile="tonal-balance",
                                    reference_role="golden", threshold=1e-4)
    flags = report["headline_flags"]
    assert len(flags) == 1 and flags[0]["flag"] == compare.FLAG_AXIS_ONLY
    assert flags[0]["expected_for"] == []                    # not a known false-alarm class
    assert "marginal or phase-only" in report["summary"]


def test_no_flags_when_axis_and_residual_agree():
    """Agreement (a real dulling both see, or identity both see) promotes NO flag."""
    ref, sr = _drum()
    dulled = compare.compare_arrays(ref, generate.dull(ref, sr), sr, reference_role="golden")
    assert dulled["headline_flags"] == []                    # both fire → corroborated → no flag
    identity = compare.compare_arrays(ref, ref.copy(), sr, reference_role="golden")
    assert identity["headline_flags"] == []                  # both quiet → corroborated → no flag


def test_headline_flags_key_always_present_even_when_empty():
    """A machine reader has ONE stable place to look — the key is always present (possibly empty)."""
    ref, sr = _drum()
    report = compare.compare_arrays(ref, ref.copy(), sr)
    assert "headline_flags" in report and report["headline_flags"] == []


def test_headline_flags_empty_on_invalid_and_not_applicable():
    ref, sr = _drum()
    bad = ref.copy(); bad[10] = np.nan
    invalid = compare.compare_arrays(ref, bad, sr)
    assert invalid["verdict"] == compare.VERDICT_INVALID and invalid["headline_flags"] == []
    short = np.random.default_rng(0).standard_normal(1024)   # < one LTAS frame
    na = compare.compare_arrays(short, short.copy(), sr)
    assert na["measurements"][0]["status"] == compare.STATUS_NOT_APPLICABLE
    assert na["headline_flags"] == []


def test_verdict_never_reads_headline_flags():
    """Hard contract: the verdict is a pure function of the primary envelope + role — a promoted
    flag (either direction) can never change it."""
    ref, sr = _drum()
    cases = [
        (np.roll(ref, 64), "golden", None),                  # uncaptured-diff flag
        (ref + 3e-4 * highband(ref), "golden", 1e-4),        # axis-only flag
    ]
    for cand, role, thr in cases:
        report = compare.compare_arrays(ref, cand, sr, profile="tonal-balance",
                                        reference_role=role, threshold=thr)
        assert report["headline_flags"]                      # a flag WAS promoted
        primary = report["measurements"][0]
        axis = compare._resolve(report["profile"])
        assert report["verdict"] == compare._verdict(axis, primary, role)   # ...but verdict is unchanged


def test_cli_prints_corroboration_status_and_flag(tmp_path, capsys):
    from quality_lab import audio_io
    ref, sr = _drum()
    cand = np.roll(ref, 64)
    ref_p, cand_p = str(tmp_path / "r.wav"), str(tmp_path / "c.wav")
    audio_io.save_wav(ref_p, ref, sr)
    audio_io.save_wav(cand_p, cand, sr)
    rc = cli.main(["compare", ref_p, cand_p, "--reference-role", "golden"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "corroboration: not_corroborated" in out
    assert "flag: uncaptured_material_difference" in out
    assert "expected for: time_variant_processing" in out
