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
