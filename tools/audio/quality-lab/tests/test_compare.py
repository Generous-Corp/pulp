"""Agent-facing `compare` report — the vertical measure→compare→judge slice.

Proves the honest agent loop on one real change (top-end dulling) with the deterministic
generators, plus the not-applicable / invalid degradation paths. Verdicts are advisory and
intent-safe: `regression_suspected` only when the reference is declared golden.
"""
from __future__ import annotations

import numpy as np

from quality_lab import audio_io, compare, generate
from quality_lab.dsp import highband


def _drum(seed: int = 0):
    y, _ = generate.render_drum_break(sr=48000, seed=seed)
    return y, 48000


def test_dulling_is_regression_when_reference_is_golden():
    ref, sr = _drum()
    cand = generate.dull(ref, sr)  # whole-signal low-pass → duller
    report = compare.compare_arrays(ref, cand, sr, reference_role="golden")
    assert report["verdict"] == compare.VERDICT_REGRESSION
    m = report["measurements"][0]
    assert m["status"] == compare.STATUS_MEASURED
    assert m["payload"]["direction"] == "duller"
    assert m["materiality"]["exceeds"] is True
    assert m["can_support_verdict"] is True


def test_dulling_is_only_material_change_for_a_peer_reference():
    """Intent-safety: a peer comparison must NOT claim regression — we don't assume which side is right."""
    ref, sr = _drum()
    cand = generate.dull(ref, sr)
    report = compare.compare_arrays(ref, cand, sr, reference_role="peer")
    assert report["verdict"] == compare.VERDICT_MATERIAL
    assert report["measurements"][0]["payload"]["direction"] == "duller"


def test_identity_is_no_material_change():
    ref, sr = _drum()
    report = compare.compare_arrays(ref, ref.copy(), sr, reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE
    assert abs(report["measurements"][0]["materiality"]["delta"]) < 0.05


def test_brighter_candidate_is_material_not_regression_even_when_golden():
    ref, sr = _drum()
    # High-frequency emphasis → clearly brighter, so even against a golden reference it is a
    # material change, not a "duller" regression.
    cand = ref + 2.0 * highband(ref)
    report = compare.compare_arrays(ref, cand, sr, reference_role="golden")
    assert report["verdict"] == compare.VERDICT_MATERIAL
    assert report["measurements"][0]["payload"]["direction"] == "brighter"


def test_nonfinite_audio_is_invalid():
    ref, sr = _drum()
    bad = ref.copy()
    bad[100] = np.nan
    report = compare.compare_arrays(ref, bad, sr)
    assert report["verdict"] == compare.VERDICT_INVALID
    assert report["measurements"][0]["status"] == compare.STATUS_INVALID


def test_silent_reference_is_not_applicable_and_inconclusive():
    sr = 48000
    silent = np.zeros(48000, dtype=np.float64)
    cand, _ = _drum()
    report = compare.compare_arrays(silent, cand[:48000], sr)
    assert report["verdict"] == compare.VERDICT_INCONCLUSIVE
    assert report["measurements"][0]["status"] == compare.STATUS_NOT_APPLICABLE


def test_too_short_material_is_not_applicable():
    sr = 48000
    short = np.random.default_rng(0).standard_normal(512)  # < one LTAS frame (2048)
    report = compare.compare_arrays(short, short.copy(), sr)
    assert report["verdict"] == compare.VERDICT_INCONCLUSIVE
    assert report["measurements"][0]["applicable"] is False


def test_unknown_profile_raises():
    ref, sr = _drum()
    try:
        compare.compare_arrays(ref, ref.copy(), sr, profile="loudness")
    except ValueError:
        return
    raise AssertionError("unknown profile should raise ValueError")


def test_compare_files_roundtrip_with_provenance(tmp_path):
    ref, sr = _drum()
    cand = generate.dull(ref, sr)
    ref_p = str(tmp_path / "ref.wav")
    cand_p = str(tmp_path / "cand.wav")
    audio_io.save_wav(ref_p, ref, sr)
    audio_io.save_wav(cand_p, cand, sr)

    report = compare.compare_files(ref_p, cand_p, reference_role="golden")
    assert report["verdict"] == compare.VERDICT_REGRESSION
    prov = report["provenance"]
    assert prov["sample_rate"] == sr
    assert len(prov["ref_sha256"]) == 64 and len(prov["cand_sha256"]) == 64
    assert prov["ref_sha256"] != prov["cand_sha256"]


def test_compare_files_sample_rate_mismatch_is_invalid(tmp_path):
    ref, _ = _drum()
    ref_p = str(tmp_path / "ref.wav")
    cand_p = str(tmp_path / "cand.wav")
    audio_io.save_wav(ref_p, ref, 48000)
    audio_io.save_wav(cand_p, ref, 44100)
    report = compare.compare_files(ref_p, cand_p)
    assert report["verdict"] == compare.VERDICT_INVALID
    assert "sample-rate mismatch" in report["measurements"][0]["reason"]


def test_compare_files_missing_input_is_invalid_not_a_crash():
    report = compare.compare_files("/nonexistent/ref.wav", "/nonexistent/cand.wav")
    assert report["verdict"] == compare.VERDICT_INVALID


def test_invalid_reports_carry_provenance_for_traceability(tmp_path):
    """An agent must be able to trace even a failed compare — invalid reports carry provenance."""
    missing = compare.compare_files("/nope/ref.wav", "/nope/cand.wav")
    assert missing["provenance"]["error"] == "decode_error"
    assert missing["provenance"]["reference"] == "/nope/ref.wav"

    ref, _ = _drum()
    ref_p, cand_p = str(tmp_path / "r.wav"), str(tmp_path / "c.wav")
    audio_io.save_wav(ref_p, ref, 48000)
    audio_io.save_wav(cand_p, ref, 44100)
    mism = compare.compare_files(ref_p, cand_p)
    assert mism["provenance"]["error"] == "sample_rate_mismatch"
    assert mism["provenance"]["ref_sample_rate"] == 48000


_COMMON_ENVELOPE_KEYS = {
    "axis", "tool", "role", "status", "applicable", "alignment", "region",
    "coverage", "can_support_verdict",
}


def test_every_envelope_shares_the_common_contract_shape():
    """measured / not_applicable / invalid envelopes must all carry the same base keys, so
    a reader (and the future MCP schema) never has to special-case the shape."""
    ref, sr = _drum()
    measured = compare.compare_arrays(ref, generate.dull(ref, sr), sr)["measurements"][0]
    not_appl = compare.compare_arrays(np.zeros(48000), ref[:48000].copy(), sr)["measurements"][0]
    bad = ref.copy(); bad[0] = np.inf
    invalid = compare.compare_arrays(ref, bad, sr)["measurements"][0]
    for env in (measured, not_appl, invalid):
        assert _COMMON_ENVELOPE_KEYS <= set(env), f"missing keys: {_COMMON_ENVELOPE_KEYS - set(env)}"
    assert not_appl["can_support_verdict"] is False
    assert invalid["can_support_verdict"] is False
    assert measured["can_support_verdict"] is True


def test_unknown_reference_role_raises():
    ref, sr = _drum()
    try:
        compare.compare_arrays(ref, ref.copy(), sr, reference_role="boss")
    except ValueError:
        return
    raise AssertionError("unknown reference_role should raise ValueError")


def test_out_of_range_threshold_raises():
    ref, sr = _drum()
    for bad in (0.0, 1.0, -0.1, 2.0):
        try:
            compare.compare_arrays(ref, ref.copy(), sr, threshold=bad)
        except ValueError:
            continue
        raise AssertionError(f"threshold {bad} should raise ValueError")


def test_nonpositive_sample_rate_raises():
    ref, _ = _drum()
    try:
        compare.compare_arrays(ref, ref.copy(), 0)
    except ValueError:
        return
    raise AssertionError("sample rate <= 0 should raise ValueError")


def test_threshold_boundary_is_material_at_exactly_threshold():
    """A shift exactly at the threshold counts as material (>=), not no-change."""
    ref, sr = _drum()
    cand = generate.dull(ref, sr)
    rel = compare.compare_arrays(ref, cand, sr)["measurements"][0]["materiality"]["delta"]
    # set threshold just under |rel| so the boundary (>=) fires material
    r = compare.compare_arrays(ref, cand, sr, reference_role="peer", threshold=abs(rel))
    assert r["measurements"][0]["materiality"]["exceeds"] is True
    assert r["verdict"] == compare.VERDICT_MATERIAL


# ── added-hf axis (the second registry entry — proves the profile registry generalizes) ──

def test_added_hf_fizz_is_regression_when_golden():
    ref, sr = _drum()
    cand = generate.add_fizz(ref, sr, amount=0.5)  # metallic high-frequency sizzle
    report = compare.compare_arrays(ref, cand, sr, profile="added-hf", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_REGRESSION
    m = report["measurements"][0]
    assert m["axis"] == "added_hf"
    assert m["payload"]["direction"] == "added HF"
    assert m["materiality"]["delta"] > 0
    assert "fizz" in report["summary"]


def test_added_hf_fizz_is_only_material_change_for_a_peer_reference():
    ref, sr = _drum()
    cand = generate.add_fizz(ref, sr, amount=0.5)
    report = compare.compare_arrays(ref, cand, sr, profile="added-hf", reference_role="peer")
    assert report["verdict"] == compare.VERDICT_MATERIAL


def test_added_hf_dulling_is_material_not_regression_even_when_golden():
    """Losing HF is a material change on the added-hf axis, but never a *regression* — only
    added fizz is the bad direction (bad_sign=+1), so a duller candidate can't trip it."""
    ref, sr = _drum()
    report = compare.compare_arrays(ref, generate.dull(ref, sr), sr,
                                    profile="added-hf", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_MATERIAL
    assert report["measurements"][0]["payload"]["direction"] == "reduced HF"


def test_added_hf_identity_is_no_material_change():
    ref, sr = _drum()
    report = compare.compare_arrays(ref, ref.copy(), sr, profile="added-hf", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE


def test_added_hf_uses_its_own_default_threshold():
    """Each axis carries its own default; added-hf's is tighter than tonal-balance's."""
    ref, sr = _drum()
    cand = generate.add_fizz(ref, sr, amount=0.5)
    report = compare.compare_arrays(ref, cand, sr, profile="added-hf")
    assert report["measurements"][0]["materiality"]["threshold"] == 0.02


def test_registry_exposes_both_profiles():
    assert set(compare.PROFILES) == {"tonal-balance", "added-hf"}


def test_added_hf_cli_profile_smoke(tmp_path, capsys):
    from quality_lab import cli
    ref, sr = _drum()
    cand = generate.add_fizz(ref, sr, amount=0.5)
    ref_p, cand_p = str(tmp_path / "r.wav"), str(tmp_path / "c.wav")
    audio_io.save_wav(ref_p, ref, sr)
    audio_io.save_wav(cand_p, cand, sr)
    rc = cli.main(["compare", ref_p, cand_p, "--profile", "added-hf",
                   "--reference-role", "golden"])
    assert rc == 0
    assert "regression_suspected" in capsys.readouterr().out


def test_verdict_keys_off_exceeds_not_rounded_delta():
    """Rounding-boundary regression guard: a raw delta just under the threshold that ROUNDS up
    to the threshold (so materiality.delta == threshold) but whose raw `exceeds` is False must
    read as no-change — the verdict keys off the raw `exceeds`, never the rounded delta."""
    axis = compare._AXES["added-hf"]
    payload = {"kind": "scalar", "hf_frac_delta": 0.02, "direction": "added HF",
               "ref_hf_frac": 0.10, "cand_hf_frac": 0.12}
    not_exceeded = compare._measurement(
        axis, compare.STATUS_MEASURED, applicable=True,
        materiality={"delta": 0.02, "unit": "hf_energy_frac_delta", "tolerance_class": "hf_fizz.v1",
                     "threshold": 0.02, "exceeds": False},
        payload=payload)
    assert compare._verdict(axis, not_exceeded, "golden") == compare.VERDICT_NO_CHANGE
    exceeded = compare._measurement(
        axis, compare.STATUS_MEASURED, applicable=True,
        materiality={"delta": 0.02, "unit": "hf_energy_frac_delta", "tolerance_class": "hf_fizz.v1",
                     "threshold": 0.02, "exceeds": True},
        payload=payload)
    assert compare._verdict(axis, exceeded, "golden") == compare.VERDICT_REGRESSION


def test_nonfinite_kernel_delta_is_invalid(monkeypatch):
    """A kernel that returns a non-finite delta must yield `invalid`, never slip into a verdict."""
    ref, sr = _drum()
    axis = compare._AXES["tonal-balance"]
    monkeypatch.setitem(
        compare._AXES, "tonal-balance",
        compare._Axis(profile=axis.profile, axis=axis.axis, tool=axis.tool,
                      default_threshold=axis.default_threshold, bad_sign=axis.bad_sign,
                      kernel=lambda m, r, s: {"applicable": True, "delta": float("nan"),
                                              "unit": "x", "tolerance_class": "t", "payload": {}},
                      summarize=axis.summarize))
    report = compare.compare_arrays(ref, ref.copy(), sr)
    assert report["verdict"] == compare.VERDICT_INVALID
    assert "non-finite delta" in report["measurements"][0]["reason"]


def test_compare_files_unknown_profile_raises_before_io():
    """Unknown profile must raise (like compare_arrays) even if the files can't be read — it
    must NOT silently fall back to tonal-balance via the decode-error path."""
    try:
        compare.compare_files("/nope/ref.wav", "/nope/cand.wav", profile="loudness")
    except ValueError:
        return
    raise AssertionError("unknown profile should raise ValueError before file I/O")


def test_cli_exit_code_is_2_only_for_invalid(tmp_path, capsys):
    from quality_lab import cli
    rc = cli.main(["compare", "/nope/ref.wav", "/nope/cand.wav"])
    assert rc == 2
    assert "invalid" in capsys.readouterr().out


def test_cli_compare_smoke(tmp_path, capsys):
    from quality_lab import cli
    ref, sr = _drum()
    cand = generate.dull(ref, sr)
    ref_p = str(tmp_path / "ref.wav")
    cand_p = str(tmp_path / "cand.wav")
    out_p = str(tmp_path / "report.json")
    audio_io.save_wav(ref_p, ref, sr)
    audio_io.save_wav(cand_p, cand, sr)

    rc = cli.main(["compare", ref_p, cand_p, "--reference-role", "golden", "--json", out_p])
    assert rc == 0  # advisory: a judgment is not a failure
    out = capsys.readouterr().out
    assert "regression_suspected" in out

    import json, os
    assert os.path.exists(out_p)
    with open(out_p) as fh:
        report = json.load(fh)
    assert report["schema"] == compare.SCHEMA
    assert report["verdict"] == compare.VERDICT_REGRESSION
