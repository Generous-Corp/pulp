"""Tier 2 — the transient-integrity compare axis (per-onset attack sharpness via the onset map).

Answers "did my DSP soften/smear the attacks?" — the phase-vocoder / dynamics artifact that peak/RMS
can't see. It detects onsets on both renders, maps them, locks each pair with sub-hop
cross-correlation, and compares the high-band attack rise (the SAME `dsp.attack_rise` primitive the
`transient_sharpness` detector uses — no forked DSP). One-directional by design: a softening is the
regression; a sharper candidate is not a transient regression. Needs onset-bearing (percussive)
material — too few matched onsets is `not_applicable`.
"""
from __future__ import annotations

import numpy as np

from quality_lab import audio_io, compare, generate


def _drum(seed: int = 0):
    y, onsets = generate.render_drum_break(sr=48000, seed=seed)
    return y, onsets, 48000


def test_smeared_attacks_are_regression_when_golden():
    ref, onsets, sr = _drum()
    cand = generate.smear_transients(ref, onsets, sr, ms=8.0)
    report = compare.compare_arrays(ref, cand, sr, profile="transient-integrity", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_REGRESSION
    m = report["measurements"][0]
    assert m["axis"] == "transient_integrity"
    assert m["materiality"]["delta"] > 0                            # positive smear deficit
    assert m["payload"]["worst_deficit"] > 0
    assert "smear" in report["summary"]


def test_smear_is_material_not_regression_for_a_peer_reference():
    ref, onsets, sr = _drum()
    cand = generate.smear_transients(ref, onsets, sr, ms=8.0)
    report = compare.compare_arrays(ref, cand, sr, profile="transient-integrity", reference_role="peer")
    assert report["verdict"] == compare.VERDICT_MATERIAL


def test_identity_is_no_material_change():
    ref, _onsets, sr = _drum()
    report = compare.compare_arrays(ref, ref.copy(), sr, profile="transient-integrity", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE
    assert report["measurements"][0]["materiality"]["delta"] == 0.0


def test_non_percussive_material_is_not_applicable():
    """A sustained tone has no onsets — the axis must be not_applicable, not a confident judgment."""
    sr = 48000
    t = np.arange(sr * 2) / sr
    sine = 0.3 * np.sin(2 * np.pi * 220.0 * t)
    report = compare.compare_arrays(sine, sine.copy(), sr, profile="transient-integrity")
    m = report["measurements"][0]
    assert m["status"] == compare.STATUS_NOT_APPLICABLE
    assert report["verdict"] == compare.VERDICT_INCONCLUSIVE
    assert "onset" in m["reason"]


def test_a_dropped_attack_is_a_regression_not_masked():
    """The most severe case the axis exists to catch: a candidate that DROPPED an attack must read
    regression, NOT no_material_change. A reference onset with no candidate onset nearby (unmatched
    or a far mis-map) scores a full deficit, never a silent skip — regression by the worst-gate."""
    ref, onsets, sr = _drum()
    cand = ref.copy()
    c, w = int(onsets[3] * sr), int(0.04 * sr)
    cand[max(0, c - w // 4): c + w] = 0.0                          # zero out one whole attack
    report = compare.compare_arrays(ref, cand, sr, profile="transient-integrity", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_REGRESSION
    p = report["measurements"][0]["payload"]
    assert p["onsets_lost"] > 0 and p["worst_deficit"] == 1.0
    assert "dropped" in report["summary"]


def test_a_tonal_change_does_not_false_fire_the_axis():
    """A tonal (non-attack) change must not spuriously read as transient damage (the two-part gate +
    the not_applicable guard keep a tonal EQ move from firing this axis)."""
    pad, _ = generate.render_tonal(sr=48000)
    report = compare.compare_arrays(pad, generate.dull(pad, 48000), 48000,
                                    profile="transient-integrity", reference_role="golden")
    assert report["verdict"] in (compare.VERDICT_NO_CHANGE, compare.VERDICT_INCONCLUSIVE)


def test_payload_reports_onset_accounting():
    ref, onsets, sr = _drum()
    cand = generate.smear_transients(ref, onsets, sr, ms=8.0)
    p = compare.compare_arrays(ref, cand, sr, profile="transient-integrity")["measurements"][0]["payload"]
    assert p["onsets_scored"] + p["onsets_lost"] <= p["onsets_detected"]
    assert p["onsets_scored"] >= 0 and p["onsets_lost"] >= 0
    assert 0.0 < p["coverage"] <= 1.0


def test_axis_shares_the_per_onset_primitive_with_the_detector():
    """`reuse, don't fork`: the compare axis and the transient_sharpness DETECTOR must use the same
    per-onset attack primitive (dsp.onset_attack_deficit), not a copied loop."""
    from quality_lab import dsp
    from quality_lab.detectors import transient_sharpness
    assert transient_sharpness.onset_attack_deficit is dsp.onset_attack_deficit


def test_global_advisory_is_suppressed_for_the_onset_axis():
    """The mono null-residual is a different (non-onset-aligned) domain, so it can't cross-check the
    per-onset attack metric — the advisory is suppressed, like stereo-width."""
    ref, onsets, sr = _drum()
    cand = generate.smear_transients(ref, onsets, sr, ms=8.0)
    report = compare.compare_arrays(ref, cand, sr, profile="transient-integrity", reference_role="golden")
    assert "advisory" not in report
    assert report["measurements"][0]["alignment"]["reason"].startswith("transient-integrity aligns")


def test_verdict_independent_of_axis_internals():
    ref, onsets, sr = _drum()
    cand = generate.smear_transients(ref, onsets, sr, ms=8.0)
    report = compare.compare_arrays(ref, cand, sr, profile="transient-integrity", reference_role="golden")
    axis = compare._resolve("transient-integrity")
    assert report["verdict"] == compare._verdict(axis, report["measurements"][0], "golden")


def test_registry_and_profile_groupings():
    assert "transient-integrity" in compare.PROFILES
    assert compare.ONSET_PROFILES == ("transient-integrity",)
    # opt-in for the net (percussive-specific), like stereo-width — not in the broad default
    assert "transient-integrity" not in compare.NET_DEFAULT_PROFILES
    assert "stereo-width" not in compare.NET_DEFAULT_PROFILES
    assert set(compare.NET_DEFAULT_PROFILES) == {"tonal-balance", "added-hf", "noise-roughness", "graininess"}


def test_cli_transient_integrity_smoke(tmp_path, capsys):
    from quality_lab import cli
    ref, onsets, sr = _drum()
    cand = generate.smear_transients(ref, onsets, sr, ms=8.0)
    ref_p, cand_p = str(tmp_path / "r.wav"), str(tmp_path / "c.wav")
    audio_io.save_wav(ref_p, ref, sr)
    audio_io.save_wav(cand_p, cand, sr)
    rc = cli.main(["compare", ref_p, cand_p, "--profile", "transient-integrity", "--reference-role", "golden"])
    assert rc == 0
    assert "regression_suspected" in capsys.readouterr().out


def test_a_pure_delay_is_no_material_change():
    """A constant delay moves every attack without touching it. Pairing onsets by order let one
    onset the candidate's detector missed shift every later pair onto a different event, which read
    a 37-sample delay as smeared attacks. Pairing is anchored on the estimated lag instead."""
    for seed in (4242, 99):
        ref, _onsets, sr = _dense_material(seed)
        for k in (1, 13, 37, 64, 101, 300):
            cand = np.concatenate([np.zeros(k), ref[: ref.size - k]])
            report = compare.compare_arrays(ref, cand, sr, profile="transient-integrity",
                                            reference_role="golden")
            assert report["verdict"] == compare.VERDICT_NO_CHANGE, (seed, k, report["summary"])


def test_a_low_noise_floor_does_not_fire_the_worst_gate():
    """Noise about 50 dB below the material leaves every attack's energy intact. The rise-time
    estimate of one attack can still jump on it, and that alone must not read as a dropped attack."""
    ref, _onsets, sr = _dense_material()
    rng = np.random.default_rng(7)
    cand = ref + rng.uniform(-0.0056, 0.0056, ref.size)
    report = compare.compare_arrays(ref, cand, sr, profile="transient-integrity",
                                    reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE, report["summary"]


def _dense_material(seed: int = 4242):
    """Six seconds of a detuned saw chord under decaying noise hits every 250 ms, through a
    2048-tap decaying-noise reverb: dense onsets, many of them weak, as in a convolved mix."""
    sr = 48000
    n = sr * 6
    t = np.arange(n) / sr
    saw = sum(2.0 * ((f * t) % 1.0) - 1.0 for f in (110.0, 164.81, 220.66))
    rng = np.random.default_rng(seed)
    hits = rng.uniform(-1.0, 1.0, n) * np.exp(-((np.arange(n) % (sr // 4)) / (0.012 * sr)))
    x = 0.18 * saw + 0.5 * hits
    ir = rng.uniform(-1.0, 1.0, 2048) * np.exp(-6.0 * np.arange(2048) / 2048) * 0.08
    ir[0] = 0.6
    y = np.convolve(x, ir)[:n]
    return y, None, sr
