"""Tier 3 / T3.1 — the `--align varispeed:R` class (declare → verify → resample-back).

A varispeed candidate is EXACTLY a resample of the reference, so resampling it back to the
reference time base undoes the speed change and lets the whole existing alignment-free pipeline —
including the phase-sensitive sample residual — measure the pair unchanged. The declaration is
VERIFIED against the observed duration ratio before it is trusted; an unsupported declaration
REFUSES (a wrong warp is worse than none). These tests drive the public compare surface + the
alignment.parse grammar.
"""
from __future__ import annotations

import numpy as np
import pytest

from quality_lab import alignment, audio_io, compare, dsp, generate


def _tonal(sr: int = 48000):
    y, _ = generate.render_tonal(sr=sr)
    return y, sr


# ── grammar (alignment.parse) ──────────────────────────────────────────────────────────────────────
def test_parse_varispeed_coerces_ratio():
    spec = alignment.parse("varispeed:1.5")
    assert spec.mode == "varispeed" and spec.param == 1.5 and spec.raw == "varispeed:1.5"


def test_parse_none_and_latency_take_no_param():
    assert alignment.parse("none").param is None
    assert alignment.parse("latency").param is None
    with pytest.raises(ValueError):
        alignment.parse("none:1")


@pytest.mark.parametrize("bad", [
    "varispeed", "varispeed:", "varispeed:abc", "varispeed:-1", "varispeed:0", "warp:1.5",
    "varispeed:inf", "varispeed:nan", "varispeed:-inf",   # non-finite must not slip past the gate
    "latency:", "none:", ":1.5",                          # trailing/empty colon is malformed
])
def test_parse_rejects_malformed_align(bad):
    with pytest.raises(ValueError):
        alignment.parse(bad)


def test_infinite_ratio_cannot_disable_the_verification_gate():
    """Regression: `varispeed:inf` must be rejected at parse — otherwise the downstream gate
    abs(observed - inf)/inf = nan, nan > tol is False, so refusal would silently never fire and the
    record would claim applied=True with declared_ratio=inf on any candidate."""
    with pytest.raises(ValueError):
        alignment.parse("varispeed:inf")


# ── the transform (through compare) ─────────────────────────────────────────────────────────────────
@pytest.mark.parametrize("ratio", [0.8, 1.25, 1.5])
@pytest.mark.parametrize("profile", ["tonal-balance", "added-hf", "noise-roughness"])
def test_clean_varispeed_reads_no_change_and_corroborated(ratio, profile):
    """A synthetic varispeed candidate, aligned by --align varispeed:R, is transparent on every axis
    AND the sample residual comes back (corroborated) — the whole existing contract is restored."""
    ref, sr = _tonal()
    cand = dsp.resample_by_ratio(ref, ratio)
    report = compare.compare_arrays(ref, cand, sr, profile=profile,
                                    align=f"varispeed:{ratio}", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE
    al = report["measurements"][0]["alignment"]
    assert al["policy"] == "varispeed-resample" and al["applied"] is True
    assert al["declared_ratio"] == ratio
    corr = (report.get("advisory") or {}).get("corroboration") or {}
    assert corr.get("status") == "corroborated"      # sample residual valid again post-resample


def test_wrong_declared_ratio_refuses():
    """Declaration sanity: declared 1.2× but the candidate is actually 1.5× — refuse, don't resample
    to a wrong length."""
    ref, sr = _tonal()
    cand = dsp.resample_by_ratio(ref, 1.5)
    report = compare.compare_arrays(ref, cand, sr, profile="tonal-balance",
                                    align="varispeed:1.2", reference_role="golden")
    al = report["measurements"][0]["alignment"]
    assert al["policy"] == "not_aligned" and al["applied"] is False
    assert "does not support" in al["reason"]
    assert al["observed_ratio"] == pytest.approx(1.5, abs=0.01)


def test_varispeed_does_not_mask_a_real_defect():
    """Anti-masking: a varispeed candidate that ALSO gained HF fizz still flags added-hf — the
    resample-back must not smear the real defect away."""
    ref, sr = _tonal()
    cand = generate.add_fizz(dsp.resample_by_ratio(ref, 1.5), sr)
    report = compare.compare_arrays(ref, cand, sr, profile="added-hf",
                                    align="varispeed:1.5", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_REGRESSION
    assert report["measurements"][0]["materiality"]["delta"] > 3.0


def test_summary_discloses_the_varispeed_alignment():
    ref, sr = _tonal()
    cand = dsp.resample_by_ratio(ref, 1.5)
    report = compare.compare_arrays(ref, cand, sr, profile="tonal-balance", align="varispeed:1.5")
    assert "varispeed" in report["summary"] and "resampling the candidate" in report["summary"]


def test_default_align_none_is_unchanged_by_the_warp_machinery():
    """The refactor + new class must not perturb the default path: align='none' still measures the
    raw pair with the exact not_required record."""
    ref, sr = _tonal()
    cand = generate.dull(ref, sr)
    report = compare.compare_arrays(ref, cand, sr, profile="tonal-balance", reference_role="golden")
    assert report["measurements"][0]["alignment"] == {"policy": "not_required", "reason": "global_ltas_metric"}


def test_varispeed_applies_to_the_onset_axis_no_false_regression():
    """A warp changes the attacks THEMSELVES, so the onset axis must NOT skip it (as it skips a
    constant lag). A clean varispeed of a drum must read no_material_change on transient-integrity —
    not a false attack-smear regression from measuring stretched attacks unaligned."""
    ref, onsets = generate.render_drum_break(sr=48000, seed=0)
    cand = dsp.resample_by_ratio(ref, 1.5)
    report = compare.compare_arrays(ref, cand, 48000, profile="transient-integrity",
                                    align="varispeed:1.5", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE
    assert report["measurements"][0]["alignment"]["policy"] == "varispeed-resample"


def test_apply_onset_axis_resamples_for_a_warp_but_skips_a_lag():
    ref, _ = generate.render_drum_break(sr=48000, seed=0)
    cand = dsp.resample_by_ratio(ref, 1.5)
    # a warp is applied (candidate resampled back to reference length) …
    _r, c, rec = alignment.apply(alignment.parse("varispeed:1.5"), ref, cand, 48000,
                                 needs_stereo=False, needs_onsets=True)
    assert rec["policy"] == "varispeed-resample" and c.size == ref.size
    # … but a constant lag is still skipped (self-aligned per onset)
    _r2, _c2, rec2 = alignment.apply(alignment.parse("latency"), ref, cand, 48000,
                                     needs_stereo=False, needs_onsets=True)
    assert rec2["policy"] == "not_required" and "aligns each onset" in rec2["reason"]


def test_stereo_axis_discloses_warp_invariance_honestly():
    """stereo-width is invariant to a uniform resample; the record must say so (not the stale
    'constant delay' reason) when a varispeed was requested."""
    ref, cand = np.zeros(2000), np.zeros(3000)
    _r, _c, rec = alignment.apply(alignment.parse("varispeed:1.5"), ref, cand, 48000,
                                  needs_stereo=True, needs_onsets=False)
    assert rec["policy"] == "not_required"
    assert "uniform time-warp" in rec["reason"] and rec["requested"] == "varispeed:1.5"


def test_a_two_percent_wrong_ratio_is_refused():
    """0.5% tolerance: a declared 1.5 against an actual 1.47 (a ~2% speed error) must refuse, not
    resample to a subtly-wrong length and claim clean."""
    ref, sr = _tonal()
    cand = dsp.resample_by_ratio(ref, 1.47)
    report = compare.compare_arrays(ref, cand, sr, profile="tonal-balance",
                                    align="varispeed:1.5", reference_role="golden")
    assert report["measurements"][0]["alignment"]["policy"] == "not_aligned"


@pytest.mark.parametrize("target", [1, 0])
def test_resample_to_degenerate_length_does_not_crash(target):
    out = dsp.resample_to_length(np.array([1.0, 2.0, 3.0, 4.0]), target)
    assert out.size == target


def test_one_sample_reference_varispeed_is_inconclusive_not_a_crash():
    """A degenerate 1-sample reference under a warp must not raise (ZeroDivisionError) — it resolves
    to a not_applicable/inconclusive report, per the compare contract."""
    report = compare.compare_arrays(np.array([1.0]), np.array([1.0, 0.5]), 48000,
                                    profile="tonal-balance", align="varispeed:2", reference_role="golden")
    assert report["verdict"] in (compare.VERDICT_INCONCLUSIVE, compare.VERDICT_INVALID)


def test_cli_varispeed_smoke(tmp_path, capsys):
    from quality_lab import cli
    ref, sr = _tonal()
    cand = dsp.resample_by_ratio(ref, 1.5)
    ref_p, cand_p = str(tmp_path / "r.wav"), str(tmp_path / "c.wav")
    audio_io.save_wav(ref_p, ref, sr)
    audio_io.save_wav(cand_p, cand, sr)
    rc = cli.main(["compare", ref_p, cand_p, "--profile", "tonal-balance",
                   "--align", "varispeed:1.5", "--reference-role", "golden"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "no_material_change_detected" in out and "varispeed" in out
