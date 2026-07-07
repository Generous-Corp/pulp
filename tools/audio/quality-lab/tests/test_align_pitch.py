"""Tier 3 / T3.3 — the `--align pitch:S` class (duration-preserving pitch shift).

A declared pitch shift leaves the time base unchanged, so the axes measure the pair directly — but
tonal-balance must COMPENSATE for the expected centroid move (a perfect S-semitone shift moves the
LTAS centroid by exactly the pitch ratio), so the axis measures the shifter's added spectral damage,
not the shift itself. The declaration is verified (|S| ≤ 24 st, duration preserved) and refused
otherwise. (Note: the shift-compensated LTAS corroborator carries log-frequency interpolation error,
so it can read `not_corroborated` on a clean shift — advisory only, never a verdict input.)
"""
from __future__ import annotations

import numpy as np
import pytest

from quality_lab import alignment, compare, dsp, generate

SR = 48000


def _harmonic(dur_s: float, f0: float) -> np.ndarray:
    """A formant-less harmonic stack: shifting f0 by a ratio is an EXACT uniform spectral scaling
    (the ground truth the pitch compensation expects), at a fixed duration (a pure pitch shift)."""
    n = int(dur_s * SR)
    t = np.arange(n) / SR
    y = np.zeros(n)
    for k in range(1, 20):
        if f0 * k < SR / 2:
            y += (1.0 / k) * np.sin(2 * np.pi * f0 * k * t)
    ar = int(0.05 * SR)
    env = np.ones(n)
    env[:ar], env[-ar:] = np.linspace(0, 1, ar), np.linspace(1, 0, ar)
    return (y * env) / (np.max(np.abs(y * env)) + 1e-12) * 0.5


# ── grammar ──────────────────────────────────────────────────────────────────────────────────────
@pytest.mark.parametrize("text,semis", [("pitch:+3", 3.0), ("pitch:-5st", -5.0),
                                        ("pitch:0", 0.0), ("pitch:12semitones", 12.0)])
def test_parse_pitch_coerces_semitones(text, semis):
    assert alignment.parse(text).param == semis


@pytest.mark.parametrize("bad", ["pitch", "pitch:", "pitch:abc", "pitch:inf", "pitch:nanst"])
def test_parse_rejects_malformed_pitch(bad):
    with pytest.raises(ValueError):
        alignment.parse(bad)


# ── the transform (through compare) ─────────────────────────────────────────────────────────────────
@pytest.mark.parametrize("semis", [3, 7, 12, -5])
def test_clean_pitch_shift_reads_no_change_after_compensation(semis):
    """A clean uniform pitch shift reads no material tonal change once compensated, whereas the
    UNCOMPENSATED centroid reads the raw pitch move (large)."""
    ratio = 2 ** (semis / 12)
    ref, cand = _harmonic(2.0, 220.0), _harmonic(2.0, 220.0 * ratio)
    comp = compare.compare_arrays(ref, cand, SR, profile="tonal-balance",
                                  align=f"pitch:{semis:+d}", reference_role="golden")
    uncomp = compare.compare_arrays(ref, cand, SR, profile="tonal-balance", align="none")
    assert comp["verdict"] == compare.VERDICT_NO_CHANGE
    m = comp["measurements"][0]
    assert m["alignment"]["policy"] == "pitch-declared" and m["alignment"]["applied"] is True
    assert m["materiality"]["tolerance_class"] == "spectral_centroid.v2-pitch"
    assert m["payload"]["pitch_ratio"] == round(ratio, 4)
    # the raw (uncompensated) centroid moved materially — compensation is what tamed it
    assert abs(uncomp["measurements"][0]["materiality"]["delta"]) > 0.15


def test_pitch_shift_plus_dulling_flags_the_dulling():
    ref = _harmonic(2.0, 220.0)
    cand = generate.dull(_harmonic(2.0, 220.0 * 2 ** (7 / 12)), SR, cutoff_hz=1500)
    report = compare.compare_arrays(ref, cand, SR, profile="tonal-balance",
                                    align="pitch:+7", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_REGRESSION
    assert report["measurements"][0]["materiality"]["delta"] < 0     # duller than the expected shift


def test_a_duration_change_is_not_a_pure_pitch_shift_and_refuses():
    ref = _harmonic(2.0, 220.0)
    report = compare.compare_arrays(ref, dsp.resample_by_ratio(ref, 1.3), SR,
                                    profile="tonal-balance", align="pitch:+3")
    al = report["measurements"][0]["alignment"]
    assert al["policy"] == "not_aligned" and "duration-preserving" in al["reason"]


@pytest.mark.parametrize("semis", [30, -36])
def test_pitch_shift_outside_domain_refuses(semis):
    ref = _harmonic(2.0, 220.0)
    report = compare.compare_arrays(ref, ref.copy(), SR, profile="tonal-balance", align=f"pitch:{semis:+d}")
    al = report["measurements"][0]["alignment"]
    assert al["policy"] == "not_aligned" and "supported range" in al["reason"]


def test_pitch_verdict_is_independent_of_the_advisory():
    ref, cand = _harmonic(2.0, 220.0), _harmonic(2.0, 220.0 * 2 ** (7 / 12))
    report = compare.compare_arrays(ref, cand, SR, profile="tonal-balance",
                                    align="pitch:+7", reference_role="golden")
    axis = compare._resolve("tonal-balance")
    assert report["verdict"] == compare._verdict(axis, report["measurements"][0], "golden")


def test_summary_discloses_the_pitch_shift():
    ref, cand = _harmonic(2.0, 220.0), _harmonic(2.0, 220.0 * 2 ** (3 / 12))
    report = compare.compare_arrays(ref, cand, SR, profile="tonal-balance", align="pitch:+3")
    assert "pitch shift" in report["summary"] and "compensated" in report["summary"]


@pytest.mark.parametrize("profile", ["added-hf", "noise-roughness", "transient-integrity", "graininess"])
def test_non_tonal_axes_decline_under_pitch_rather_than_false_flag(profile):
    """A clean pitch shift legitimately moves HF energy / the HNR pitch-lag / the attack high band, so
    only tonal-balance is compensated. The other axes must DECLINE (not_applicable) under --align
    pitch, never confidently report a clean shift as a regression."""
    ref, cand = _harmonic(2.0, 220.0), _harmonic(2.0, 220.0 * 2 ** (12 / 12))
    report = compare.compare_arrays(ref, cand, SR, profile=profile, align="pitch:+12", reference_role="golden")
    m = report["measurements"][0]
    assert m["status"] == compare.STATUS_NOT_APPLICABLE
    assert report["verdict"] == compare.VERDICT_INCONCLUSIVE
    assert "pitch-invariant" in m["reason"]


def test_stereo_width_under_pitch_declines_not_bypasses_verification():
    """Regression (Codex): stereo-width's capability short-circuit must not let an unverified pitch
    declaration through as `no_material_change` — the axis declines under pitch."""
    ref = _harmonic(2.0, 220.0)
    st = np.column_stack([ref, ref])
    report = compare.compare_arrays(ref, ref.copy(), SR, profile="stereo-width",
                                    align="pitch:+30", stereo=(st, st))
    assert report["measurements"][0]["status"] == compare.STATUS_NOT_APPLICABLE
    assert report["verdict"] != compare.VERDICT_NO_CHANGE


def test_pitch_zero_is_a_noop_equivalent_to_none():
    """Regression (Codex): a declared zero-semitone shift is a no-op — it must behave like `none`
    (raw pair, sample-domain corroborator that still catches a delay), not swap in the delay-blind LTAS
    corroborator and suppress a genuine offset."""
    ref = _harmonic(2.0, 220.0)
    delayed = np.concatenate([np.zeros(4000), ref[:-4000]])
    p0 = compare.compare_arrays(ref, delayed, SR, profile="tonal-balance", align="pitch:0")
    none = compare.compare_arrays(ref, delayed, SR, profile="tonal-balance", align="none")
    assert p0["measurements"][0]["alignment"] == none["measurements"][0]["alignment"]
    assert (p0.get("advisory") or {}).get("corroboration", {}).get("basis", {}).get("raw_comparator") \
        == "quality-lab:null_residual"   # sample residual, not the LTAS corroborator


def test_cli_pitch_smoke(tmp_path, capsys):
    from quality_lab import audio_io, cli
    ref, cand = _harmonic(2.0, 220.0), _harmonic(2.0, 220.0 * 2 ** (3 / 12))
    ref_p, cand_p = str(tmp_path / "r.wav"), str(tmp_path / "c.wav")
    audio_io.save_wav(ref_p, ref, SR)
    audio_io.save_wav(cand_p, cand, SR)
    rc = cli.main(["compare", ref_p, cand_p, "--profile", "tonal-balance", "--align", "pitch:+3"])
    assert rc == 0
    assert "pitch" in capsys.readouterr().out
