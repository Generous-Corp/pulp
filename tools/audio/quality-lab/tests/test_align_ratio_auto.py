"""Tier 3 / T3.4 — `--align ratio:auto` (uniform-ratio ESTIMATION, double-gated).

When you don't know the stretch ratio, `ratio:auto` estimates it — but only when two INDEPENDENT
estimators agree: the duration ratio (candidate/reference length) and the onset-time slope (Theil-Sen
over matched onsets). Agreement → it applies the estimated ratio through the ordinary stretch path;
disagreement or too few onsets → it REFUSES (a single-estimator ratio is an unverifiable guess). It
reliably estimates onset-bearing uniform EXPANSIONS; compressions and ambiguous material refuse
(declare stretch:R for those).
"""
from __future__ import annotations

import numpy as np
import pytest

from quality_lab import alignment, compare, dsp, generate

SR = 48000


def _click_train(times: list[float], dur_s: float) -> np.ndarray:
    """A pitch-preserving, onset-bearing signal: the SAME click at each of `times`, so scaling the
    times (and duration) is an exact clean uniform stretch — the ground truth ratio:auto must recover."""
    n = int(dur_s * SR)
    y = np.zeros(n)
    click = np.hanning(200)[:100] * np.sin(2 * np.pi * 3000 * np.arange(100) / SR)
    for t in times:
        i = int(t * SR)
        if 0 <= i < n - 100:
            y[i:i + 100] += click
    ar = int(0.02 * SR)
    env = np.ones(n)
    env[:ar], env[-ar:] = np.linspace(0, 1, ar), np.linspace(1, 0, ar)
    return (y * env) / (np.max(np.abs(y)) + 1e-9) * 0.5


def _stretched_pair(ratio: float, n_onsets: int = 10, base_dur: float = 2.5):
    times = [0.15 + 0.18 * i for i in range(n_onsets)]
    return _click_train(times, base_dur), _click_train([ratio * t for t in times], base_dur * ratio)


# ── grammar ──────────────────────────────────────────────────────────────────────────────────────
def test_parse_ratio_auto():
    assert alignment.parse("ratio:auto").param == "auto"


@pytest.mark.parametrize("bad", ["ratio", "ratio:1.5", "ratio:", "ratio:auto:2"])
def test_parse_rejects_non_auto_ratio(bad):
    with pytest.raises(ValueError):
        alignment.parse(bad)


# ── estimation (through compare) ──────────────────────────────────────────────────────────────────
@pytest.mark.parametrize("ratio", [1.5, 2.0])
def test_estimates_a_uniform_expansion_and_reads_no_change(ratio):
    ref, cand = _stretched_pair(ratio)
    report = compare.compare_arrays(ref, cand, SR, profile="tonal-balance", align="ratio:auto", reference_role="golden")
    al = report["measurements"][0]["alignment"]
    assert al["policy"] == "stretch-declared" and al["applied"] is True
    assert al["estimated"] is True
    assert al["declared_ratio"] == pytest.approx(ratio, rel=0.03)
    assert report["verdict"] == compare.VERDICT_NO_CHANGE


def test_ratio_auto_matches_the_declared_stretch_path():
    ref, cand = _stretched_pair(1.5)
    auto = compare.compare_arrays(ref, cand, SR, profile="tonal-balance", align="ratio:auto", reference_role="golden")
    declared = compare.compare_arrays(ref, cand, SR, profile="tonal-balance", align="stretch:1.5", reference_role="golden")
    assert auto["verdict"] == declared["verdict"]


def test_graininess_hop_scales_off_the_estimated_ratio():
    """The estimated ratio must flow to the warp-aware axes (via effective_spec), not the 'auto'
    request string — graininess measures spectral_flux.v2-warp under ratio:auto."""
    ref, cand = _stretched_pair(1.5)
    m = compare.compare_arrays(ref, cand, SR, profile="graininess", align="ratio:auto")["measurements"][0]
    assert m["materiality"]["tolerance_class"] == "spectral_flux.v2-warp"


def test_non_uniform_warp_refuses_estimators_disagree():
    drum, _ = generate.render_drum_break(sr=SR, seed=0)
    half = len(drum) // 2
    nonuni = np.concatenate([dsp.resample_by_ratio(drum[:half], 1.2), dsp.resample_by_ratio(drum[half:], 1.8)])
    report = compare.compare_arrays(drum, nonuni, SR, profile="tonal-balance", align="ratio:auto")
    al = report["measurements"][0]["alignment"]
    assert al["policy"] == "not_aligned"
    assert "disagree" in al["reason"] or "non-uniform" in al["reason"]


def test_a_single_grossly_displaced_onset_refuses():
    """Auto-estimation guards residual spread the robust slope/MAD would otherwise absorb: one onset
    displaced far off the uniform line (a local non-uniformity) must refuse, not read as clean."""
    times = [0.15 + 0.18 * i for i in range(10)]
    cand_times = [1.5 * t for t in times]
    cand_times[2] += 0.25                                   # one onset shoved 250 ms off the line
    ref, cand = _click_train(times, 2.5), _click_train(cand_times, 2.5 * 1.5)
    report = compare.compare_arrays(ref, cand, SR, profile="tonal-balance", align="ratio:auto")
    al = report["measurements"][0]["alignment"]
    assert al["policy"] == "not_aligned"
    assert al["worst_onset_residual_ms"] > 100 and "not uniform" in al["reason"]


def test_onset_jitter_within_tolerance_still_estimates():
    """The residual guard is lenient — small onset-detection jitter must NOT refuse a clean stretch."""
    times = [0.15 + 0.18 * i for i in range(10)]
    cand_times = [1.5 * t + (0.008 if i % 2 else -0.008) for i, t in enumerate(times)]  # ±8 ms jitter
    ref, cand = _click_train(times, 2.5), _click_train(cand_times, 2.5 * 1.5)
    al = compare.compare_arrays(ref, cand, SR, profile="tonal-balance", align="ratio:auto")["measurements"][0]["alignment"]
    assert al["policy"] == "stretch-declared" and al["estimated"] is True


def test_too_few_onsets_refuses():
    """Sparse/sustained material can't verify the estimate with a second estimator → refuse rather
    than trust a single-estimator guess."""
    ref, cand = _stretched_pair(1.5, n_onsets=3)   # below the 6-onset minimum
    report = compare.compare_arrays(ref, cand, SR, profile="tonal-balance", align="ratio:auto")
    al = report["measurements"][0]["alignment"]
    assert al["policy"] == "not_aligned" and "onset" in al["reason"]


def test_refused_ratio_auto_is_not_invalid():
    ref, cand = _stretched_pair(1.5, n_onsets=3)
    report = compare.compare_arrays(ref, cand, SR, profile="tonal-balance", align="ratio:auto")
    assert report["verdict"] != compare.VERDICT_INVALID     # a refusal is measured unaligned, not invalid


def test_summary_discloses_the_estimate():
    ref, cand = _stretched_pair(1.5)
    report = compare.compare_arrays(ref, cand, SR, profile="graininess", align="ratio:auto")
    assert "Estimated" in report["summary"] and "agree" in report["summary"]


def test_cli_ratio_auto_smoke(tmp_path, capsys):
    from quality_lab import audio_io, cli
    ref, cand = _stretched_pair(1.5)
    ref_p, cand_p = str(tmp_path / "r.wav"), str(tmp_path / "c.wav")
    audio_io.save_wav(ref_p, ref, SR)
    audio_io.save_wav(cand_p, cand, SR)
    rc = cli.main(["compare", ref_p, cand_p, "--profile", "tonal-balance", "--align", "ratio:auto"])
    assert rc == 0
    assert "Estimated" in capsys.readouterr().out
