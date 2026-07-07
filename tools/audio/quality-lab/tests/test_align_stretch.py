"""Tier 3 / T3.2 — the `--align stretch:R` class (pitch-preserving time-stretch).

Unlike varispeed, a pitch-preserving stretch has no exact inverse to apply, so the axes measure the
UNWARPED pair directly (LTAS/HNR/width are time-average, warp-invariant). Two axis-level
normalizations, keyed off the declared ratio: graininess uses hop-scaled flux (else a clean stretch
reads a false "smoother"), and corroboration binds to the phase-blind LTAS distance (the sample
residual is invalid across a stretch). The declaration is VERIFIED (§6.1 duration + §6.3 that a single
uniform ratio fits) and REFUSED otherwise.
"""
from __future__ import annotations

import numpy as np
import pytest

from quality_lab import alignment, compare, dsp, generate

SR = 48000


def _clean_tonal(dur_s: float, f0: float = 220.0, n_vib: float = 12.0, n_trem: float = 2.0) -> np.ndarray:
    """A tonal signal that is EXACTLY clean-stretchable: constant carrier pitch, vibrato/tremolo
    indexed by normalized position, so `_clean_tonal(D*R)` is a clean pitch-preserving stretch by R
    of `_clean_tonal(D)`."""
    n = int(dur_s * SR)
    t = np.arange(n) / SR
    p = t / dur_s
    vib = 1.0 + 0.01 * np.sin(2 * np.pi * n_vib * p)
    phase = 2 * np.pi * f0 * np.cumsum(vib) / SR
    y = np.zeros(n)
    for k in range(1, 14):
        hz = f0 * k
        formant = np.exp(-(((hz - 700.0) / 400.0) ** 2)) + 0.7 * np.exp(-(((hz - 1800.0) / 600.0) ** 2))
        y += (1.0 / k) * (0.35 + formant) * np.sin(k * phase)
    y *= 0.85 + 0.15 * np.sin(2 * np.pi * n_trem * p)
    ar = int(0.05 * SR)
    env = np.ones(n)
    env[:ar], env[-ar:] = np.linspace(0, 1, ar), np.linspace(1, 0, ar)
    return (y * env) / (np.max(np.abs(y * env)) + 1e-12) * 0.5


# ── grammar ──────────────────────────────────────────────────────────────────────────────────────
def test_parse_stretch_coerces_ratio():
    spec = alignment.parse("stretch:1.5")
    assert spec.mode == "stretch" and spec.param == 1.5


@pytest.mark.parametrize("bad", ["stretch", "stretch:", "stretch:0", "stretch:-1", "stretch:inf", "stretch:abc"])
def test_parse_rejects_malformed_stretch(bad):
    with pytest.raises(ValueError):
        alignment.parse(bad)


# ── the transform (through compare) ─────────────────────────────────────────────────────────────────
@pytest.mark.parametrize("profile", ["tonal-balance", "added-hf", "graininess"])
def test_clean_stretch_reads_no_change_and_corroborated(profile):
    ref, cand = _clean_tonal(2.5), _clean_tonal(2.5 * 1.5)
    report = compare.compare_arrays(ref, cand, SR, profile=profile, align="stretch:1.5", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE
    al = report["measurements"][0]["alignment"]
    assert al["policy"] == "stretch-declared" and al["applied"] is True
    corr = (report.get("advisory") or {}).get("corroboration") or {}
    assert corr.get("status") == "corroborated"
    assert corr["basis"]["raw_comparator"] == "quality-lab:ltas_residual"   # warp-compatible corroborator


def test_graininess_hop_scaling_removes_the_false_smoother():
    """Under a clean stretch the un-scaled candidate flux reads a false 'smoother' (negative rel); the
    hop-scaled v2-warp path brings it back near zero."""
    ref, cand = _clean_tonal(2.5), _clean_tonal(2.5 * 1.5)
    unscaled = compare.compare_arrays(ref, cand, SR, profile="graininess", align="none")
    scaled = compare.compare_arrays(ref, cand, SR, profile="graininess", align="stretch:1.5")
    assert unscaled["measurements"][0]["materiality"]["delta"] < -0.1          # false smoother
    m = scaled["measurements"][0]
    assert abs(m["materiality"]["delta"]) < 0.1                                 # corrected
    assert m["materiality"]["tolerance_class"] == "spectral_flux.v2-warp"       # warp-normalized path
    assert m["payload"]["flux_hop_scale"] == 1.5


def test_grainy_stretch_flags_graininess():
    ref, cand = _clean_tonal(2.5), generate.grainy(_clean_tonal(2.5 * 1.5), SR)
    report = compare.compare_arrays(ref, cand, SR, profile="graininess", align="stretch:1.5", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_REGRESSION
    assert report["measurements"][0]["materiality"]["delta"] > 0.5


def test_stretch_does_not_mask_added_fizz():
    ref, cand = _clean_tonal(2.5), generate.add_fizz(_clean_tonal(2.5 * 1.5), SR)
    report = compare.compare_arrays(ref, cand, SR, profile="added-hf", align="stretch:1.5", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_REGRESSION


def test_wrong_declared_ratio_refuses():
    ref, cand = _clean_tonal(2.5), _clean_tonal(2.5 * 1.5)   # actually 1.5×
    report = compare.compare_arrays(ref, cand, SR, profile="tonal-balance", align="stretch:1.2")
    assert report["measurements"][0]["alignment"]["policy"] == "not_aligned"


@pytest.mark.parametrize("ratio", [0.1, 5.0])
def test_ratio_outside_domain_refuses(ratio):
    ref, cand = _clean_tonal(2.5), _clean_tonal(2.5)
    report = compare.compare_arrays(ref, cand, SR, profile="tonal-balance", align=f"stretch:{ratio}")
    al = report["measurements"][0]["alignment"]
    assert al["policy"] == "not_aligned" and "supported range" in al["reason"]


@pytest.mark.parametrize("ratio", [0.3, 0.5, 0.75])
def test_uniform_compression_is_accepted_not_false_refused(ratio):
    """Regression: a clean uniform COMPRESSION (R<1) must be accepted. A compression's onset
    refractory folds close onsets together, which broke a strict 1:1 onset map; the prediction-nearest
    residual is robust to it. (This whole R<1 domain previously false-refused and silently reverted the
    warp normalization.)"""
    drum, _onsets = generate.render_drum_break(sr=SR, seed=0)
    warped = dsp.resample_by_ratio(drum, ratio)
    report = compare.compare_arrays(drum, warped, SR, profile="tonal-balance", align=f"stretch:{ratio}")
    assert report["measurements"][0]["alignment"]["policy"] == "stretch-declared"


def test_uniform_onset_warp_accepts_but_non_uniform_refuses():
    """§6.3: a single uniform ratio must fit. A uniformly warped drum is accepted; a piecewise
    (first-half 1.2×, second-half 1.8×) warp declared uniform is refused with the onset-scatter reason."""
    drum, _onsets = generate.render_drum_break(sr=SR, seed=0)
    uniform = dsp.resample_by_ratio(drum, 1.5)
    ok = compare.compare_arrays(drum, uniform, SR, profile="tonal-balance", align="stretch:1.5")
    assert ok["measurements"][0]["alignment"]["policy"] == "stretch-declared"

    half = len(drum) // 2
    nonuni = np.concatenate([dsp.resample_by_ratio(drum[:half], 1.2), dsp.resample_by_ratio(drum[half:], 1.8)])
    obs = len(nonuni) / len(drum)
    bad = compare.compare_arrays(drum, nonuni, SR, profile="tonal-balance", align=f"stretch:{obs:.4f}")
    al = bad["measurements"][0]["alignment"]
    assert al["policy"] == "not_aligned" and "non-uniformly warped" in al["reason"]


def test_ltas_corroborator_is_phase_blind_and_emitted_alongside_null():
    """The stretch advisory emits BOTH the (now-invalid) null_residual and the warp-compatible
    ltas_residual, and the corroboration binds to the latter."""
    ref, cand = _clean_tonal(2.5), _clean_tonal(2.5 * 1.5)
    report = compare.compare_arrays(ref, cand, SR, profile="tonal-balance", align="stretch:1.5")
    raws = {r["name"] for r in report["advisory"]["raw_comparators"]}
    assert {"null_residual", "ltas_residual"} <= raws


def test_summary_discloses_the_stretch():
    ref, cand = _clean_tonal(2.5), _clean_tonal(2.5 * 1.5)
    report = compare.compare_arrays(ref, cand, SR, profile="graininess", align="stretch:1.5")
    assert "stretch" in report["summary"] and "warp-normalized" in report["summary"]


def test_cli_stretch_smoke(tmp_path, capsys):
    from quality_lab import audio_io, cli
    ref, cand = _clean_tonal(2.5), _clean_tonal(2.5 * 1.5)
    ref_p, cand_p = str(tmp_path / "r.wav"), str(tmp_path / "c.wav")
    audio_io.save_wav(ref_p, ref, SR)
    audio_io.save_wav(cand_p, cand, SR)
    rc = cli.main(["compare", ref_p, cand_p, "--profile", "graininess", "--align", "stretch:1.5"])
    assert rc == 0
    assert "stretch" in capsys.readouterr().out
