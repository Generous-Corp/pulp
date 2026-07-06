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


def _hf_atten(y, sr, atten_db=-26.0, corner_hz=2000.0):
    """Attenuate energy >= corner_hz by atten_db via a spectral shelf — turns the bright drum
    into a bass-heavy 'amp analog' source with a small (nonzero) >=8 kHz fraction, where the
    band-relative HF metric earns its keep (the absolute fraction is near-blind on such a source)."""
    Y = np.fft.rfft(y); f = np.fft.rfftfreq(len(y), 1.0 / sr)
    return np.fft.irfft(Y * np.where(f >= corner_hz, 10 ** (atten_db / 20.0), 1.0), n=len(y))


def _lf_shelf(y, sr, boost_db, corner_hz=500.0):
    """Boost energy below corner_hz by boost_db — a broadband EQ move OUTSIDE the HF band."""
    Y = np.fft.rfft(y); f = np.fft.rfftfreq(len(y), 1.0 / sr)
    return np.fft.irfft(Y * np.where(f < corner_hz, 10 ** (boost_db / 20.0), 1.0), n=len(y))


def _brickwall_lpf(y, sr, cutoff_hz):
    """Hard low-pass — a candidate LPF'd below the 8 kHz cutoff has ~zero HF fraction."""
    Y = np.fft.rfft(y); f = np.fft.rfftfreq(len(y), 1.0 / sr)
    Y[f >= cutoff_hz] = 0.0
    return np.fft.irfft(Y, n=len(y))


def _bass_heavy(seed: int = 0):
    """A bass-heavy render (small nonzero >=8 kHz fraction) — the added-hf axis's real target."""
    y, sr = _drum(seed)
    return _hf_atten(y, sr), sr


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
    ref, sr = _bass_heavy()                          # bass-heavy amp analog: HF fraction ~0.004
    cand = generate.add_fizz(ref, sr, amount=0.3)    # metallic high-frequency sizzle (~+15 dB band-rel)
    report = compare.compare_arrays(ref, cand, sr, profile="added-hf", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_REGRESSION
    m = report["measurements"][0]
    assert m["axis"] == "added_hf"
    assert m["payload"]["direction"] == "added HF"
    assert m["materiality"]["delta"] > 0             # band-relative dB, positive = brighter/harsher
    assert m["materiality"]["unit"] == "hf_fraction_ratio_db"
    assert "fizz" in report["summary"]


def test_added_hf_fizz_is_only_material_change_for_a_peer_reference():
    ref, sr = _bass_heavy()
    cand = generate.add_fizz(ref, sr, amount=0.3)
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
    """Each axis carries its own default in its own unit; added-hf's is a dB magnitude."""
    ref, sr = _bass_heavy()
    cand = generate.add_fizz(ref, sr, amount=0.3)
    report = compare.compare_arrays(ref, cand, sr, profile="added-hf")
    m = report["measurements"][0]["materiality"]
    assert m["threshold"] == 3.0
    assert m["unit"] == "hf_fraction_ratio_db"
    assert m["tolerance_class"] == "hf_fizz.v2"


def test_registry_exposes_all_profiles():
    assert set(compare.PROFILES) == {"tonal-balance", "added-hf", "noise-roughness",
                                     "graininess", "stereo-width"}
    assert compare.STEREO_PROFILES == ("stereo-width",)
    assert "stereo-width" not in compare.MONO_PROFILES


def test_added_hf_cli_profile_smoke(tmp_path, capsys):
    from quality_lab import cli
    ref, sr = _bass_heavy()
    cand = generate.add_fizz(ref, sr, amount=0.3)
    ref_p, cand_p = str(tmp_path / "r.wav"), str(tmp_path / "c.wav")
    audio_io.save_wav(ref_p, ref, sr)
    audio_io.save_wav(cand_p, cand, sr)
    rc = cli.main(["compare", ref_p, cand_p, "--profile", "added-hf",
                   "--reference-role", "golden"])
    assert rc == 0
    assert "regression_suspected" in capsys.readouterr().out


def test_added_hf_is_broadband_gain_invariant():
    """The band-relative fraction-RATIO is invariant to a broadband gain / the level-match — a
    candidate that is just the reference scaled reads no material change (the whole reason the
    metric replaced the absolute ≥8 kHz fraction delta, which the level-match dragged around)."""
    ref, sr = _bass_heavy()
    report = compare.compare_arrays(ref, ref * 2.0, sr, profile="added-hf", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE
    assert abs(report["measurements"][0]["materiality"]["delta"]) < 3.0


def test_added_hf_moderate_lf_shelf_is_not_material():
    """An EQ move OUTSIDE the HF band (a moderate +3 dB LF shelf) must not read as an HF change —
    the fraction ratio removes the broadband-gain component the absolute metric would mistake for
    'reduced HF'. (Large shelves still leave residual sensitivity, documented in the dsp metric.)"""
    ref, sr = _bass_heavy()
    cand = _lf_shelf(ref, sr, boost_db=2.0)           # ~-1.9 dB band-relative, comfortably < 3 dB
    report = compare.compare_arrays(ref, cand, sr, profile="added-hf", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE
    assert abs(report["measurements"][0]["materiality"]["delta"]) < 3.0


def test_added_hf_zero_hf_reference_is_finite_material_not_invalid():
    """Fizz added to a dark (zero-HF) source: the reference HF fraction is at the floor, so a naive
    ratio would be +inf → invalid → exit 2. Floor-clamping both sides yields a large FINITE dB
    delta and a normal 'added HF' verdict — never invalid."""
    sr = 48000
    t = np.arange(sr) / sr
    dark = 0.3 * np.sin(2 * np.pi * 200.0 * t)       # a low tone: ~no >=8 kHz energy
    cand = generate.add_fizz(dark, sr, amount=0.5)
    report = compare.compare_arrays(dark, cand, sr, profile="added-hf", reference_role="golden")
    m = report["measurements"][0]
    assert m["status"] == compare.STATUS_MEASURED
    assert np.isfinite(m["materiality"]["delta"]) and m["materiality"]["delta"] > 0
    assert report["verdict"] == compare.VERDICT_REGRESSION


def test_added_hf_zero_hf_candidate_is_finite_material_not_invalid():
    """A brickwall low-pass leaves the candidate with ~zero HF: the candidate fraction is at the
    floor, so a naive ratio would be -inf → invalid → exit 2. Floor-clamping yields a large FINITE
    negative dB delta and a normal 'reduced HF' verdict."""
    ref, sr = _drum()                                # bright source (HF fraction ~0.58)
    cand = _brickwall_lpf(ref, sr, cutoff_hz=4000.0)  # removes all >=8 kHz energy
    report = compare.compare_arrays(ref, cand, sr, profile="added-hf", reference_role="golden")
    m = report["measurements"][0]
    assert m["status"] == compare.STATUS_MEASURED
    assert np.isfinite(m["materiality"]["delta"]) and m["materiality"]["delta"] < 0
    assert m["payload"]["direction"] == "reduced HF"
    assert report["verdict"] == compare.VERDICT_MATERIAL   # HF loss is material but not a regression


def test_verdict_direction_uses_raw_sign_not_rounded_delta():
    """Regression-DIRECTION guard (Codex retrospective must-fix): with a threshold finer than the
    4-decimal rounding of `materiality.delta`, a tiny BRIGHTER change (raw delta > 0) whose delta
    rounds to 0.0 must NOT be read as a 'duller' regression. The verdict keys off the raw
    `direction_sign`, so it stays consistent with the payload direction."""
    ref, sr = _drum()
    cand = ref + 5e-8 * highband(ref)                 # a hair brighter; |rel| rounds to 0.0000
    report = compare.compare_arrays(ref, cand, sr, profile="tonal-balance",
                                    reference_role="golden", threshold=1e-8)
    m = report["measurements"][0]
    assert m["materiality"]["exceeds"] is True        # it IS a (tiny) material change
    assert m["materiality"]["direction_sign"] == 1    # raw sign = brighter
    assert m["payload"]["direction"] == "brighter"
    assert report["verdict"] == compare.VERDICT_MATERIAL   # brighter is NOT the duller-regression direction
    assert report["verdict"] != compare.VERDICT_REGRESSION


def test_dc_present_needs_absolute_floor_not_just_ratio():
    """A near-silent signal has a meaningful mean/RMS RATIO but a negligible absolute DC that rounds
    to 0.0 in the report — it must NOT raise a 'DC offset present' advisory over displayed zeros."""
    sr = 48000
    near = np.ones(48000) * 1.1e-9                    # tiny constant: ratio ~1 but |mean| < abs floor
    report = compare.compare_arrays(near, near.copy(), sr, reference_role="peer")
    dc = next(r for r in report["advisory"]["raw_comparators"] if r["name"] == "dc_offset")
    assert dc["detail"]["present"] is False
    assert "DC offset" not in report["summary"]


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
                     "threshold": 0.02, "exceeds": True, "direction_sign": 1},
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


# ── Slice 1: honesty fixes (downmix disclosure, added-hf applicability, DC-offset) ────────

def test_stereo_candidate_downmix_is_disclosed(tmp_path):
    """A stereo candidate whose MID equals the reference reads 'no material change' on the mono
    axis — but the report MUST disclose that it folded to mono and never saw the stereo image,
    rather than silently returning a clean verdict (honesty-per-measurement)."""
    ref, sr = _drum()
    side = 0.5 * highband(ref)                       # decorrelated side content (a widener)
    cand_stereo = np.stack([ref + side, ref - side], axis=1)  # mid == ref, but now wide
    ref_p, cand_p = str(tmp_path / "r.wav"), str(tmp_path / "c.wav")
    audio_io.save_wav(ref_p, ref, sr)
    audio_io.save_wav(cand_p, cand_stereo, sr)

    report = compare.compare_files(ref_p, cand_p, reference_role="golden")
    assert report["provenance"]["ref_channels"] == 1
    assert report["provenance"]["cand_channels"] == 2
    m = report["measurements"][0]
    assert m["downmix"]["applied"] is True
    assert m["downmix"]["cand_channels"] == 2
    assert "downmix" in report["summary"].lower()
    # The mono downmix is ~identical to the reference, so the axis itself sees no material change.
    assert report["verdict"] == compare.VERDICT_NO_CHANGE


def test_mono_inputs_carry_no_downmix_note(tmp_path):
    """Mono-in/mono-out must NOT add a downmix note — the disclosure only fires on real folding."""
    ref, sr = _drum()
    report = compare.compare_arrays(ref, ref.copy(), sr, input_channels=(1, 1))
    assert "downmix" not in report["measurements"][0]
    assert "downmix" not in report["summary"].lower()


def test_invalid_report_keeps_downmix_field_but_drops_prose_clause():
    """On the invalid path the structured `downmix` field still discloses the fold, but the prose
    clause is suppressed — 'downmixed to mono' reads oddly on a report that compared nothing."""
    ref, sr = _drum()
    bad = ref.copy(); bad[100] = np.nan
    report = compare.compare_arrays(ref, bad, sr, input_channels=(2, 2))
    assert report["verdict"] == compare.VERDICT_INVALID
    assert report["measurements"][0]["downmix"]["applied"] is True   # structured disclosure kept
    assert "downmix" not in report["summary"].lower()                # prose clause suppressed


def test_added_hf_not_applicable_when_band_too_narrow():
    """At sr=16 kHz the >=8 kHz band is the single Nyquist bin — the added-hf axis must report
    `not_applicable`, never a confident 'no material change' over a degenerate band (F4)."""
    sr = 16000
    y = np.random.default_rng(0).standard_normal(sr) * 0.1  # >= one LTAS frame, not silent
    report = compare.compare_arrays(y, y.copy(), sr, profile="added-hf")
    m = report["measurements"][0]
    assert m["status"] == compare.STATUS_NOT_APPLICABLE
    assert report["verdict"] == compare.VERDICT_INCONCLUSIVE
    assert "band too narrow" in m["reason"]


def test_added_hf_applicable_at_normal_rate():
    """Guard sanity: a normal rate (48 kHz) keeps the axis fully applicable."""
    ref, sr = _drum()
    m = compare.compare_arrays(ref, ref.copy(), sr, profile="added-hf")["measurements"][0]
    assert m["status"] == compare.STATUS_MEASURED


def test_dc_offset_is_surfaced_and_cross_referenced():
    """A pure DC offset drags the centroid down and reads as 'duller' (F5). The DC-offset raw
    comparator must be present and `present=True`, and the summary must cross-reference it so an
    agent doesn't hunt phantom dullness — all advisory, never moving the verdict."""
    ref, sr = _drum()
    cand = ref + 0.1                                  # pure DC offset, no timbral change
    report = compare.compare_arrays(ref, cand, sr, reference_role="golden")
    raws = report["advisory"]["raw_comparators"]
    assert raws[0]["name"] == "null_residual"         # residual stays at index 0 (corroboration contract)
    dc = next(r for r in raws if r["name"] == "dc_offset")
    assert dc["participates_in_verdict"] is False
    assert dc["detail"]["present"] is True
    assert "DC offset" in report["summary"]


def test_clean_inputs_have_no_dc_cross_reference():
    """No DC offset → the comparator is present (informational) but `present=False`, and the
    summary carries no DC caveat."""
    ref, sr = _drum()
    report = compare.compare_arrays(ref, generate.dull(ref, sr), sr, reference_role="golden")
    dc = next(r for r in report["advisory"]["raw_comparators"] if r["name"] == "dc_offset")
    assert dc["detail"]["present"] is False
    assert "DC offset" not in report["summary"]


def test_dc_offset_does_not_move_the_verdict():
    """Verdict-independence: the verdict computed from the primary measurement + role must equal
    the report's verdict even though a DC offset added a summary caveat and a raw comparator."""
    ref, sr = _drum()
    cand = ref + 0.1
    report = compare.compare_arrays(ref, cand, sr, reference_role="golden")
    axis = compare._AXES["tonal-balance"]
    recomputed = compare._verdict(axis, report["measurements"][0], "golden")
    assert report["verdict"] == recomputed


def test_cli_unknown_profile_is_clean_invalid_not_argparse_exit(tmp_path, capsys):
    """An unknown --profile must produce a clean structured `invalid` report + exit 2, NOT an
    argparse exit-2 with no report — otherwise a delegating caller (the MCP tool) reads the empty
    report as 'tool not installed'. The valid set lives in the Python registry, surfaced here."""
    from quality_lab import cli
    ref, sr = _drum()
    ref_p, cand_p = str(tmp_path / "r.wav"), str(tmp_path / "c.wav")
    out_p = str(tmp_path / "report.json")
    audio_io.save_wav(ref_p, ref, sr)
    audio_io.save_wav(cand_p, ref, sr)
    rc = cli.main(["compare", ref_p, cand_p, "--profile", "loudness", "--json", out_p])
    assert rc == 2                                        # invalid exit, not an argparse SystemExit
    out = capsys.readouterr().out
    assert "invalid" in out
    import json, os
    assert os.path.exists(out_p)                          # a structured report WAS written
    with open(out_p) as fh:
        report = json.load(fh)
    assert report["verdict"] == compare.VERDICT_INVALID
    assert "unknown profile" in report["summary"]         # names the bad profile + the valid set


def test_cli_out_of_range_threshold_is_clean_invalid_not_traceback(tmp_path, capsys):
    """An out-of-range --threshold must produce a clean `invalid` report + exit 2, NOT an uncaught
    ValueError traceback — otherwise the MCP's empty-report heuristic misreads it as 'tool absent'.
    (added-hf's dB range is (0, 60), so 999 is out of range.)"""
    from quality_lab import cli
    ref, sr = _drum()
    ref_p, cand_p = str(tmp_path / "r.wav"), str(tmp_path / "c.wav")
    out_p = str(tmp_path / "report.json")
    audio_io.save_wav(ref_p, ref, sr)
    audio_io.save_wav(cand_p, ref, sr)
    rc = cli.main(["compare", ref_p, cand_p, "--profile", "added-hf",
                   "--threshold", "999", "--json", out_p])
    assert rc == 2
    out = capsys.readouterr().out
    assert "invalid" in out
    import json, os
    assert os.path.exists(out_p)
    with open(out_p) as fh:
        report = json.load(fh)
    assert report["verdict"] == compare.VERDICT_INVALID
    assert "must be in" in report["summary"]           # the range message is surfaced, not swallowed


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
