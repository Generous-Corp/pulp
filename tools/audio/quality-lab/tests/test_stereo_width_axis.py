"""The stereo-width compare axis — closes the gap where a stereo change was invisible to compare.

compare mean-downmixes to mono, so a widener/panner/collapse used to read as "no change" (Slice 1
only DISCLOSED that). This axis measures the ORIGINAL 2-channel signal: RMS(side)/RMS(mid) width
plus interchannel correlation. Mono input on either side is not_applicable; a collapse toward mono
is the regression direction; a candidate gone out of phase is surfaced.
"""
from __future__ import annotations

import numpy as np

from quality_lab import audio_io, compare, generate


def _pad(seed: int = 0):
    return generate.render_stereo_pad(sr=48000, seed=seed), 48000     # (N, 2)


def _widen(stereo, amount):
    mid = 0.5 * (stereo[:, 0] + stereo[:, 1])
    side = 0.5 * (stereo[:, 0] - stereo[:, 1]) * amount
    return np.stack([mid + side, mid - side], axis=1)


def _write(tmp_path, name, y, sr):
    p = str(tmp_path / name)
    audio_io.save_wav(p, y, sr)
    return p


def test_registry_exposes_five_profiles():
    assert set(compare.PROFILES) == {"tonal-balance", "added-hf", "noise-roughness",
                                     "graininess", "stereo-width"}


def test_stereo_collapse_is_regression_when_golden(tmp_path):
    st, sr = _pad()
    golden = _write(tmp_path, "g.wav", st, sr)
    cand = _write(tmp_path, "c.wav", generate.narrow_stereo(st, amount=1.0), sr)   # collapse to mono
    report = compare.compare_files(golden, cand, profile="stereo-width", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_REGRESSION
    m = report["measurements"][0]
    assert m["axis"] == "stereo_width"
    assert m["materiality"]["delta"] < 0                  # width dropped (narrowed)
    assert m["payload"]["direction"] == "narrower"
    assert report["provenance"]["ref_channels"] == 2
    assert "downmix" not in m                             # this axis DID measure the stereo image


def test_stereo_widen_is_material_not_regression(tmp_path):
    st, sr = _pad()
    golden = _write(tmp_path, "g.wav", st, sr)
    cand = _write(tmp_path, "c.wav", _widen(st, 2.0), sr)
    report = compare.compare_files(golden, cand, profile="stereo-width", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_MATERIAL   # wider is a material change, not a regression
    assert report["measurements"][0]["payload"]["direction"] == "wider"


def test_stereo_identity_is_no_change(tmp_path):
    st, sr = _pad()
    golden = _write(tmp_path, "g.wav", st, sr)
    cand = _write(tmp_path, "c.wav", st.copy(), sr)
    report = compare.compare_files(golden, cand, profile="stereo-width", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE


def test_mono_input_is_not_applicable(tmp_path):
    """Both inputs must be stereo — a mono compare on this axis is honestly not_applicable."""
    mono, _ = generate.render_tonal(sr=48000)
    golden = _write(tmp_path, "g.wav", mono, 48000)
    cand = _write(tmp_path, "c.wav", mono.copy(), 48000)
    report = compare.compare_files(golden, cand, profile="stereo-width", reference_role="golden")
    assert report["measurements"][0]["status"] == compare.STATUS_NOT_APPLICABLE
    assert report["verdict"] == compare.VERDICT_INCONCLUSIVE
    assert "2-channel" in report["measurements"][0]["reason"]


def test_one_sided_mono_is_not_applicable(tmp_path):
    """A stereo golden vs a mono candidate (or vice versa) is also not_applicable — the width axis
    needs 2 channels on BOTH sides."""
    st, sr = _pad()
    mono = 0.5 * (st[:, 0] + st[:, 1])
    golden = _write(tmp_path, "g.wav", st, sr)
    cand = _write(tmp_path, "c.wav", mono, sr)
    report = compare.compare_files(golden, cand, profile="stereo-width", reference_role="golden")
    assert report["measurements"][0]["status"] == compare.STATUS_NOT_APPLICABLE


def test_phase_flip_is_surfaced_in_payload_and_summary(tmp_path):
    """Flipping a channel's polarity is a mono-compatibility defect the width delta alone can't name
    (it pushes width UP). The negative interchannel correlation must show in the payload + summary."""
    st, sr = _pad()
    golden = _write(tmp_path, "g.wav", st, sr)
    cand = _write(tmp_path, "c.wav", generate.invert_phase_right(st), sr)
    report = compare.compare_files(golden, cand, profile="stereo-width", reference_role="golden")
    m = report["measurements"][0]
    assert m["payload"]["cand_corr"] < 0                  # out of phase
    assert m["payload"]["ref_corr"] > 0
    assert "out of phase" in report["summary"]


def test_stereo_widener_with_unchanged_mid_is_caught_only_by_this_axis(tmp_path):
    """The gap-closer: a widener whose MID equals the golden reads NO material change on the mono
    tonal axis (identical downmix) but a clear material change on stereo-width — the stereo change
    compare used to be blind to is now caught."""
    st, sr = _pad()
    golden = _write(tmp_path, "g.wav", st, sr)
    wider = _write(tmp_path, "c.wav", _widen(st, 2.0), sr)               # mid identical, side doubled
    mono_view = compare.compare_files(golden, wider, profile="tonal-balance", reference_role="golden")
    assert mono_view["verdict"] == compare.VERDICT_NO_CHANGE             # mono axis is blind...
    stereo_view = compare.compare_files(golden, wider, profile="stereo-width", reference_role="golden")
    assert stereo_view["verdict"] == compare.VERDICT_MATERIAL            # ...the stereo axis is not


def test_stereo_width_verdict_independent_of_advisory(tmp_path):
    st, sr = _pad()
    golden = _write(tmp_path, "g.wav", st, sr)
    cand = _write(tmp_path, "c.wav", generate.narrow_stereo(st, amount=1.0), sr)
    report = compare.compare_files(golden, cand, profile="stereo-width", reference_role="golden")
    primary = report["measurements"][0]
    axis = compare._resolve("stereo-width")
    assert report["verdict"] == compare._verdict(axis, primary, "golden")


def test_stereo_axis_has_no_misleading_mono_corroboration(tmp_path):
    """The mono null-residual is a DIFFERENT domain and can't cross-check a stereo metric — a pure
    widener (mid unchanged) makes the mono residual read identity and would wrongly cast the real
    stereo change as 'phase-only'. The advisory (and its headline flags) must be suppressed here."""
    st, sr = _pad()
    golden = _write(tmp_path, "g.wav", st, sr)
    wider = _write(tmp_path, "c.wav", _widen(st, 2.0), sr)              # mid identical, side doubled
    report = compare.compare_files(golden, wider, profile="stereo-width", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_MATERIAL
    assert "advisory" not in report                                    # no mono cross-check
    assert report["headline_flags"] == []                              # ...so no misleading flag
    assert compare.FLAG_AXIS_ONLY not in report["summary"]


def test_malformed_stereo_tuple_is_not_applicable_not_a_crash():
    """A direct caller passing a non-(N,2) stereo tuple gets a clean not_applicable, not a raw
    ValueError out of the dsp primitive (compare_files never passes such a tuple)."""
    sr = 48000
    mono = np.random.default_rng(0).standard_normal(48000) * 0.1
    bad = np.zeros((48000, 1))                                          # (N,1), not (N,2)
    report = compare.compare_arrays(mono, mono.copy(), sr, profile="stereo-width",
                                    stereo=(bad, bad))
    assert report["measurements"][0]["status"] == compare.STATUS_NOT_APPLICABLE
    assert report["verdict"] == compare.VERDICT_INCONCLUSIVE


def test_cli_stereo_width_smoke(tmp_path, capsys):
    from quality_lab import cli
    st, sr = _pad()
    golden = _write(tmp_path, "g.wav", st, sr)
    cand = _write(tmp_path, "c.wav", generate.narrow_stereo(st, amount=1.0), sr)
    rc = cli.main(["compare", golden, cand, "--profile", "stereo-width", "--reference-role", "golden"])
    assert rc == 0
    assert "regression_suspected" in capsys.readouterr().out
