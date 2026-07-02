"""Slice 4 — the noise-roughness (HNR) and graininess (spectral-flux) compare axes.

These answer the questions a NAM/amp or effect dev actually asks — "did it get rougher/noisier?
grainier?" — on tonal/sustained material (a caller-declared contract surfaced as a summary caveat,
NOT an unvalidated tonal/percussive classifier in the honesty path). They mirror the existing axes'
contract: level-match -> global metric -> signed delta -> intent-safe verdict, with only
mathematically degenerate inputs going not_applicable.
"""
from __future__ import annotations

import numpy as np

from quality_lab import compare, generate


def _pad(seed: int = 0):
    y, _ = generate.render_tonal(sr=48000)               # (audio, onsets) — sr is fixed at 48k
    return y, 48000


# ── noise-roughness (HNR) ────────────────────────────────────────────────────────────────

def test_noise_roughness_is_regression_when_golden():
    ref, sr = _pad()
    cand = generate.noisy(ref, sr, amount=0.12)          # broadband noise -> HNR drops ~5 dB
    report = compare.compare_arrays(ref, cand, sr, profile="noise-roughness", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_REGRESSION
    m = report["measurements"][0]
    assert m["axis"] == "noise_roughness"
    assert m["materiality"]["delta"] < 0                 # HNR dropped (rougher)
    assert m["payload"]["direction"] == "rougher/noisier"
    assert "tonal/sustained" in report["summary"]        # material-contract caveat present


def test_noise_roughness_peer_is_material_not_regression():
    ref, sr = _pad()
    cand = generate.noisy(ref, sr, amount=0.12)
    report = compare.compare_arrays(ref, cand, sr, profile="noise-roughness", reference_role="peer")
    assert report["verdict"] == compare.VERDICT_MATERIAL


def test_noise_roughness_identity_is_no_change():
    ref, sr = _pad()
    report = compare.compare_arrays(ref, ref.copy(), sr, profile="noise-roughness", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE


def test_noise_roughness_default_threshold_and_unit():
    ref, sr = _pad()
    m = compare.compare_arrays(ref, generate.noisy(ref, sr, amount=0.12), sr,
                               profile="noise-roughness")["measurements"][0]["materiality"]
    assert m["threshold"] == 1.5 and m["unit"] == "hnr_delta_db"


# ── graininess (spectral flux) ───────────────────────────────────────────────────────────

def test_graininess_is_regression_when_golden():
    ref, sr = _pad()
    cand = generate.grainy(ref, sr, amount=0.18)         # frame-to-frame churn -> flux rises
    report = compare.compare_arrays(ref, cand, sr, profile="graininess", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_REGRESSION
    m = report["measurements"][0]
    assert m["axis"] == "graininess"
    assert m["materiality"]["delta"] > 0                 # relative flux increased (grainier)
    assert m["payload"]["direction"] == "grainier"
    assert "tonal/sustained" in report["summary"]


def test_graininess_identity_is_no_change():
    ref, sr = _pad()
    report = compare.compare_arrays(ref, ref.copy(), sr, profile="graininess", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE


def test_graininess_not_applicable_when_reference_has_no_flux():
    """A constant (DC) reference has ~zero spectral flux — the relative-increase metric would be a
    div-by-zero. That degenerate case (and only that) goes not_applicable, honestly."""
    sr = 48000
    const = np.ones(48000) * 0.3                          # not silent, but zero frame-to-frame change
    report = compare.compare_arrays(const, const.copy(), sr, profile="graininess")
    m = report["measurements"][0]
    assert m["status"] == compare.STATUS_NOT_APPLICABLE
    assert report["verdict"] == compare.VERDICT_INCONCLUSIVE
    assert "spectral flux" in m["reason"]


def test_graininess_default_threshold_and_unit():
    ref, sr = _pad()
    m = compare.compare_arrays(ref, generate.grainy(ref, sr, amount=0.18), sr,
                               profile="graininess")["measurements"][0]["materiality"]
    assert m["threshold"] == 0.5 and m["unit"] == "rel_flux_increase"


# ── both wired axes carry the advisory + never move the verdict ──────────────────────────

def test_new_axes_verdict_independent_of_advisory():
    ref, sr = _pad()
    for profile, cand in [("noise-roughness", generate.noisy(ref, sr, amount=0.12)),
                          ("graininess", generate.grainy(ref, sr, amount=0.18))]:
        report = compare.compare_arrays(ref, cand, sr, profile=profile, reference_role="golden")
        primary = report["measurements"][0]
        axis = compare._resolve(profile)
        assert report["verdict"] == compare._verdict(axis, primary, "golden")
