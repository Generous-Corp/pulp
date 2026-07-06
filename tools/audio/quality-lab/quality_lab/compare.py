"""Agent-facing before/after comparison report (advisory, off-gate).

The vertical `compare` surface: given a reference and a candidate render, it level-matches,
runs one curated **axis** (selected by `--profile`), and emits a typed *evidence envelope*
plus a defended, action-oriented verdict. It exists so an agent tuning DSP can measure a
change, compare before/after, and weigh in with cited evidence — not a bare pass/fail.

Axes are registered in `_AXES`: each is a reference-vs-candidate metric that mirrors the same
contract, so adding one is a single registry entry (the shared `_measure`/`_verdict` machinery does
level-matching, applicability, materiality, and the intent-safe verdict). Most are global and
alignment-free (`tonal-balance`, `added-hf`, `noise-roughness`, `graininess`); `stereo-width` reads
the original 2-channel signal (`needs_stereo`); `transient-integrity` self-aligns per onset
(`needs_onsets`). An optional `--align latency` step trims a constant delay before measuring. Axes
that need a controlled stimulus (THD/response) or a non-constant time-warp are deferred (see the
alignment plan).

Boundaries (see planning/2026-07-01-agent-audio-measurement-substrate.md and the audio-harness
skill for the full rationale):
- **Advisory, never a gate.** `pulp audio validate compare` remains the gate primitive.
- **Schema is owned by `schema.py`** (`quality_lab.compare.v1`); this module owns the axis logic.
- **Intent-safe verdicts.** `regression_suspected` is only emitted when the caller declares the
  reference known-good (`reference_role="golden"`) AND the change is in the axis's *bad* direction;
  a `peer` comparison is always the neutral `material_change_detected`.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import Any, Callable

import numpy as np

from . import audio_io, schema
from .align import detect_onsets, map_onsets
from .dsp import (
    apply_lag_trim,
    dc_offset_metrics,
    estimate_global_lag,
    harmonic_to_noise_ratio_db,
    hf_band_bin_count,
    hf_fraction_ratio_db,
    interchannel_correlation,
    mean_spectral_flux,
    null_residual_db,
    onset_attack_deficit,
    relative_centroid_shift,
    stereo_width_ratio,
)

# Re-export the schema's compare vocabulary so the agent-facing API is one import.
SCHEMA = schema.COMPARE_SCHEMA
VERDICT_REGRESSION = schema.COMPARE_VERDICT_REGRESSION
VERDICT_MATERIAL = schema.COMPARE_VERDICT_MATERIAL
VERDICT_NO_CHANGE = schema.COMPARE_VERDICT_NO_CHANGE
VERDICT_INCONCLUSIVE = schema.COMPARE_VERDICT_INCONCLUSIVE
VERDICT_INVALID = schema.COMPARE_VERDICT_INVALID
STATUS_MEASURED = schema.COMPARE_STATUS_MEASURED
STATUS_NOT_APPLICABLE = schema.COMPARE_STATUS_NOT_APPLICABLE
STATUS_INVALID = schema.COMPARE_STATUS_INVALID
CORROBORATED = schema.COMPARE_CORROBORATED
NOT_CORROBORATED = schema.COMPARE_NOT_CORROBORATED
CORROBORATION_NA = schema.COMPARE_CORROBORATION_NA
FLAG_UNCAPTURED_DIFF = schema.COMPARE_FLAG_UNCAPTURED_DIFF
FLAG_AXIS_ONLY = schema.COMPARE_FLAG_AXIS_ONLY

_LTAS_N_FFT = 2048  # a valid LTAS needs at least this many samples

# Alignment modes for `--align` (opt-in; default "none" preserves every existing contract). "latency"
# estimates a single constant lag and trims to a common time base so a pure delay/offset reads as the
# tone change it is, not a false "material" from the shift. Below this confidence floor the difference
# is NOT a pure delay, so we REFUSE to trim (a wrong alignment is worse than none) and record
# `not_aligned`. Floor 0.7 sits above spurious partial/out-of-range matches (~0.68) yet well below
# real delays (~0.9-1.0); ambiguous (periodic) and boundary peaks are already forced to confidence 0
# in the estimator. Future modes (onset/ratio/dtw) plug in here behind the same alignment record.
_ALIGN_MODES = ("none", "latency")
_ALIGN_CONFIDENCE_FLOOR = 0.7

# added-hf band. Below `_HF_MIN_BINS` LTAS bins at/above the cutoff, the >=cutoff band is too
# narrow for an energy *fraction* to mean anything (e.g. sr=16 kHz → the >=8 kHz band is the
# single Nyquist bin), so the axis reports `not_applicable` instead of a confident fraction. 8
# bins cleanly separates degenerate rates (<=~16 kHz) from every normal one (>=22.05 kHz has
# hundreds of bins), keeping the guard honest without excluding a real working rate.
_HF_CUTOFF_HZ = 8000.0
_HF_MIN_BINS = 8
# HNR (noise-roughness) pitch range: 40 Hz covers bass fundamentals (low E ~41 Hz) — the default
# 70 Hz would miss them, precisely the material the compare axis is asked about. flux (graininess)
# needs a non-degenerate reference; below this floor the reference has no sustained content to
# measure churn against, so the axis is not_applicable (the div-by-~zero edge, cf. Slice 2).
_HNR_FMIN, _HNR_FMAX = 40.0, 1000.0
_FLUX_REF_FLOOR = 1e-9
# noise-roughness and graininess are only meaningful on tonal/sustained material — a caller-declared
# contract (the caller picks the profile), surfaced as a standing summary caveat rather than an
# unvalidated tonal/percussive CLASSIFIER inside the honesty path (which would just be a tunable
# threshold on the same statistic). Both axes still carry per-signal scalars in the payload so a
# reader can judge, and only mathematically degenerate inputs go not_applicable.
_MATERIAL_CAVEAT = " (meaningful on tonal/sustained material)"
# Floor for the band-relative HF fraction-ratio: a fraction at/below this (~-60 dB, far under any
# real HF fraction) clamps so a zero-HF reference or candidate yields a large FINITE dB delta, not
# -inf → an `invalid` report (which would violate "nonzero exit only when we could not measure").
_HF_FRACTION_FLOOR = 1e-6
# Stereo width is RMS(side)/RMS(mid); its delta between renders. A collapse toward mono is the
# regression direction (bad_sign=-1). Interchannel correlation rides in the payload + a summary note:
# a candidate whose correlation goes negative is out of phase (mono-incompatible) — a defect the
# width delta alone (which a phase flip pushes *up*) does not name.
_STEREO_WIDTH_THRESHOLD = 0.1
# transient-integrity needs enough judgeable onsets to measure attack sharpness — below this the
# material is not percussive/onset-bearing enough and the axis is not_applicable (a caller-declared
# material contract like roughness/graininess, surfaced honestly rather than guessed).
_ONSET_MIN_MATCHED = 3
# A reference onset whose mapped candidate onset is farther than this (normalized time) is a
# dropped/moved attack, not the same event — scored as a FULL deficit (1.0), never silently skipped.
# A dropped attack is the maximal transient loss the axis exists to catch.
_ONSET_MATCH_TOL_NORM = 0.05
# A single onset softened at least this much fires the verdict even when the mean is diluted across
# many faithful onsets (the sparse-catastrophe gate) — while a lone noisy onset below it does not.
_WORST_HARD_DEFICIT = 0.5

# Above this level-matched, reference-relative residual, an algorithm-agnostic sample-domain
# difference is present (the two renders are not effectively identical). Heuristic floor: -60 dB
# rel means the residual is ~1/1000 of the signal RMS. It gates *materiality* only, never the
# verdict — see `_corroboration`. A length mismatch needs NO separate threshold: null_residual_db
# zero-pads the shorter signal, so a dropped/added tail with content raises the residual on its
# own (and trailing silence does not), keeping the identity claim honest for any render length.
_RESIDUAL_MATERIAL_DB = -60.0
_RAW_NULL_TOOL = "quality-lab:null_residual"
_RAW_DC_TOOL = "quality-lab:dc_offset"


# ── Axis registry ───────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class _Axis:
    """One comparison axis. `kernel` computes the metric on the (level-matched) candidate vs
    reference and returns either `{"applicable": False, "reason": …}` or `{"applicable": True,
    "delta", "unit", "tolerance_class", "payload"}`. `bad_sign` is the sign of `delta` that is a
    regression against a *golden* reference (-1 = a drop is bad, +1 = an increase is bad)."""
    profile: str          # CLI --profile value
    axis: str             # envelope axis id
    tool: str
    default_threshold: float
    bad_sign: int
    kernel: Callable[[np.ndarray, np.ndarray, int], dict[str, Any]]
    summarize: Callable[[str, dict[str, Any]], str]
    # Valid (exclusive) threshold range for THIS axis's unit. Both current axes are
    # dimensionless fractions in (0, 1); a future dB-unit axis declares its own range here
    # so the guard travels with the axis instead of being baked into `compare_arrays`.
    threshold_range: tuple[float, float] = (0.0, 1.0)
    # When True the axis measures per-onset attacks (transient-integrity): it self-aligns each onset,
    # so the global `--align` step is skipped and the global sample-domain advisory (a different,
    # non-onset-aligned domain) is suppressed. Its kernel does onset detect/match/attack internally.
    needs_onsets: bool = False
    # When True the kernel is fed the ORIGINAL 2-channel signals (candidate, reference), not the mono
    # downmix — the stereo-image axes measure L/R relationships the mono path throws away. `compare`
    # supplies the stereo pair; mono input makes such an axis not_applicable.
    needs_stereo: bool = False


def _generic_head(verdict: str, env: dict[str, Any]) -> str | None:
    if verdict == VERDICT_INVALID:
        return f"Measurement invalid: {env.get('reason', 'could not analyze inputs')}."
    if verdict == VERDICT_INCONCLUSIVE:
        return f"Inconclusive: {env.get('reason', 'measurement could not support a verdict')}."
    return None


def _centroid_kernel(matched: np.ndarray, reference: np.ndarray, sr: int) -> dict[str, Any]:
    rel, c_ref, c_cand = relative_centroid_shift(reference, matched, sr)
    if c_ref <= 1e-9:
        return {"applicable": False, "reason": "reference centroid undefined (no spectral energy)"}
    direction = "duller" if rel < 0 else "brighter"
    return {"applicable": True, "delta": rel, "unit": "rel_centroid_shift",
            "tolerance_class": "spectral_centroid.v1",
            "payload": {"kind": "scalar", "ref_centroid_hz": round(c_ref, 1),
                        "cand_centroid_hz": round(c_cand, 1), "rel_shift": round(rel, 4),
                        "direction": direction}}


def _tonal_summary(verdict: str, env: dict[str, Any]) -> str:
    head = _generic_head(verdict, env)
    if head:
        return head
    p = env["payload"]
    shift = (f"LTAS centroid {p['ref_centroid_hz']:.0f}->{p['cand_centroid_hz']:.0f} Hz "
             f"({p['direction']} {abs(p['rel_shift'])*100:.1f}%)")
    if verdict == VERDICT_NO_CHANGE:
        return f"No material tonal-balance change ({shift})."
    if verdict == VERDICT_REGRESSION:
        return f"Regression suspected: candidate is materially duller than the golden reference ({shift})."
    return f"Material tonal-balance change: candidate is {p['direction']} ({shift})."


def _added_hf_kernel(matched: np.ndarray, reference: np.ndarray, sr: int) -> dict[str, Any]:
    n_hf = hf_band_bin_count(sr, _HF_CUTOFF_HZ, _LTAS_N_FFT)
    if n_hf < _HF_MIN_BINS:
        return {"applicable": False,
                "reason": (f"high-frequency band too narrow at {sr} Hz: only {n_hf} LTAS "
                           f"bin(s) >= {_HF_CUTOFF_HZ:.0f} Hz (need >= {_HF_MIN_BINS})")}
    ratio_db, hf_ref, hf_cand = hf_fraction_ratio_db(reference, matched, sr, _HF_CUTOFF_HZ, _HF_FRACTION_FLOOR)
    direction = "added HF" if ratio_db > 0 else "reduced HF"
    return {"applicable": True, "delta": ratio_db, "unit": "hf_fraction_ratio_db",
            "tolerance_class": "hf_fizz.v2",
            "payload": {"kind": "scalar", "ref_hf_frac": round(hf_ref, 6),
                        "cand_hf_frac": round(hf_cand, 6), "hf_ratio_db": round(ratio_db, 2),
                        "direction": direction}}


def _added_hf_summary(verdict: str, env: dict[str, Any]) -> str:
    head = _generic_head(verdict, env)
    if head:
        return head
    p = env["payload"]
    band = (f"HF(>=8kHz) fraction {p['ref_hf_frac']:.4g}->{p['cand_hf_frac']:.4g} "
            f"(band-relative {p['hf_ratio_db']:+.1f} dB)")
    if verdict == VERDICT_NO_CHANGE:
        return f"No material high-frequency balance change ({band})."
    if verdict == VERDICT_REGRESSION:
        return f"Regression suspected: candidate added metallic high-frequency fizz vs the golden reference ({band})."
    return f"Material high-frequency balance change: candidate {p['direction']} ({band})."


def _noise_roughness_kernel(matched: np.ndarray, reference: np.ndarray, sr: int) -> dict[str, Any]:
    hnr_ref = harmonic_to_noise_ratio_db(reference, sr, fmin=_HNR_FMIN, fmax=_HNR_FMAX)
    hnr_cand = harmonic_to_noise_ratio_db(matched, sr, fmin=_HNR_FMIN, fmax=_HNR_FMAX)
    delta = hnr_cand - hnr_ref              # dB; a DROP (negative) = added noise/roughness = bad
    direction = "rougher/noisier" if delta < 0 else "cleaner"
    return {"applicable": True, "delta": delta, "unit": "hnr_delta_db",
            "tolerance_class": "hnr.v1",
            "payload": {"kind": "scalar", "ref_hnr_db": round(hnr_ref, 2),
                        "cand_hnr_db": round(hnr_cand, 2), "hnr_delta_db": round(delta, 2),
                        "direction": direction}}


def _noise_roughness_summary(verdict: str, env: dict[str, Any]) -> str:
    head = _generic_head(verdict, env)
    if head:
        return head
    p = env["payload"]
    m = f"HNR {p['ref_hnr_db']:.1f}->{p['cand_hnr_db']:.1f} dB (delta {p['hnr_delta_db']:+.1f} dB)"
    if verdict == VERDICT_NO_CHANGE:
        return f"No material roughness change ({m}){_MATERIAL_CAVEAT}."
    if verdict == VERDICT_REGRESSION:
        return f"Regression suspected: candidate is rougher/noisier than the golden reference ({m}){_MATERIAL_CAVEAT}."
    return f"Material roughness change: candidate is {p['direction']} ({m}){_MATERIAL_CAVEAT}."


def _graininess_kernel(matched: np.ndarray, reference: np.ndarray, sr: int) -> dict[str, Any]:
    flux_ref = mean_spectral_flux(reference, sr)
    if flux_ref <= _FLUX_REF_FLOOR:
        return {"applicable": False,
                "reason": "reference has negligible spectral flux (no sustained content to measure graininess against)"}
    flux_cand = mean_spectral_flux(matched, sr)
    rel = (flux_cand - flux_ref) / flux_ref   # relative flux increase; a RISE (positive) = grainier = bad
    direction = "grainier" if rel > 0 else "smoother"
    return {"applicable": True, "delta": rel, "unit": "rel_flux_increase",
            "tolerance_class": "spectral_flux.v1",
            "payload": {"kind": "scalar", "ref_flux": round(flux_ref, 5),
                        "cand_flux": round(flux_cand, 5), "rel_flux_increase": round(rel, 3),
                        "direction": direction}}


def _graininess_summary(verdict: str, env: dict[str, Any]) -> str:
    head = _generic_head(verdict, env)
    if head:
        return head
    p = env["payload"]
    m = f"spectral flux {p['ref_flux']:.4g}->{p['cand_flux']:.4g} (rel {p['rel_flux_increase']:+.0%})"
    if verdict == VERDICT_NO_CHANGE:
        return f"No material graininess change ({m}){_MATERIAL_CAVEAT}."
    if verdict == VERDICT_REGRESSION:
        return f"Regression suspected: candidate is grainier than the golden reference ({m}){_MATERIAL_CAVEAT}."
    return f"Material graininess change: candidate is {p['direction']} ({m}){_MATERIAL_CAVEAT}."


def _stereo_width_kernel(cand_stereo: np.ndarray, ref_stereo: np.ndarray, sr: int) -> dict[str, Any]:
    w_ref, w_cand = stereo_width_ratio(ref_stereo), stereo_width_ratio(cand_stereo)
    c_ref, c_cand = interchannel_correlation(ref_stereo), interchannel_correlation(cand_stereo)
    delta = w_cand - w_ref                     # RMS(side)/RMS(mid) change; a drop = narrowing/collapse
    direction = "wider" if delta > 0 else "narrower"
    return {"applicable": True, "delta": delta, "unit": "stereo_width_delta",
            "tolerance_class": "stereo_width.v1",
            "payload": {"kind": "scalar", "ref_width": round(w_ref, 4), "cand_width": round(w_cand, 4),
                        "width_delta": round(delta, 4), "ref_corr": round(c_ref, 3),
                        "cand_corr": round(c_cand, 3), "direction": direction}}


def _stereo_width_summary(verdict: str, env: dict[str, Any]) -> str:
    head = _generic_head(verdict, env)
    if head:
        return head
    p = env["payload"]
    m = (f"width(side/mid) {p['ref_width']:.3f}->{p['cand_width']:.3f} (delta {p['width_delta']:+.3f}), "
         f"interchannel corr {p['ref_corr']:+.2f}->{p['cand_corr']:+.2f}")
    # A candidate correlation gone negative is out of phase (mono-incompatible) — a defect the width
    # delta does not name (a phase flip pushes width UP). Surface it whenever it newly appears.
    phase = (" Candidate is out of phase (interchannel correlation is negative) — a mono-compatibility defect."
             if p["cand_corr"] < 0 <= p["ref_corr"] else "")
    if verdict == VERDICT_NO_CHANGE:
        return f"No material stereo-width change ({m}).{phase}"
    if verdict == VERDICT_REGRESSION:
        return f"Regression suspected: stereo image narrowed/collapsed vs the golden reference ({m}).{phase}"
    return f"Material stereo-width change: image is {p['direction']} ({m}).{phase}"


def _transient_integrity_kernel(matched: np.ndarray, reference: np.ndarray, sr: int) -> dict[str, Any]:
    """Per-onset attack-sharpness ratio of candidate vs reference. Detects onsets on both signals,
    maps them (align.map_onsets), locks each pair with sub-hop cross-correlation (dsp.local_align),
    and compares the high-band attack rise (dsp.attack_rise — the SAME primitive the
    transient_sharpness detector uses). Aggregates to the mean cand/ref sharpness ratio; delta =
    ratio − 1 (signed: < 0 = attacks softened/smeared). not_applicable on too few matched onsets."""
    ref_onsets = detect_onsets(reference, sr)
    if len(ref_onsets) < _ONSET_MIN_MATCHED:
        return {"applicable": False,
                "reason": (f"too few onsets in the reference ({len(ref_onsets)} < {_ONSET_MIN_MATCHED}) "
                           "— transient-integrity needs percussive/onset-bearing material")}
    cand_onsets = detect_onsets(matched, sr)
    ref_dur, cand_dur = reference.size / sr, matched.size / sr
    mapped = dict(map_onsets(ref_onsets, cand_onsets, ref_dur, cand_dur))   # {ref_t: cand_t}
    # Score EVERY reference onset. A per-onset deficit is clip(1 − s_cand/s_ref, 0, 1): 0 = faithful
    # (or sharper), 1 = fully softened. Crucially, a reference attack with NO candidate onset nearby
    # (unmatched, or the mapped one is a different event > tol away) is DROPPED/moved → a full
    # deficit, never silently skipped — a dropped drum hit is the maximal transient loss the axis
    # exists to catch. Only an unmeasurable window (boundary / silent reference attack) is skipped.
    deficits: list[float] = []
    lost = scored = skipped = 0
    for ref_t in ref_onsets:
        cand_t = mapped.get(ref_t)
        if cand_t is None or abs(cand_t / cand_dur - ref_t / ref_dur) > _ONSET_MATCH_TOL_NORM:
            deficits.append(1.0)
            lost += 1
            continue
        d = onset_attack_deficit(reference, matched, sr, ref_t, cand_t)
        if d is None:
            skipped += 1
            continue
        deficits.append(d)
        scored += 1
    if len(deficits) < _ONSET_MIN_MATCHED:
        return {"applicable": False,
                "reason": f"too few judgeable onsets ({len(deficits)} < {_ONSET_MIN_MATCHED})"}
    mean_deficit, worst_deficit = float(np.mean(deficits)), float(np.max(deficits))
    # Two-part severity: a single catastrophic loss fires via `worst` even when diluted across many
    # faithful onsets; a broad gentle smear fires via `mean`; a lone noisy onset does not false-fire.
    delta = worst_deficit if worst_deficit >= _WORST_HARD_DEFICIT else mean_deficit
    direction = "softer/smeared attacks" if delta > 1e-4 else "faithful attacks"
    return {"applicable": True, "delta": delta, "unit": "attack_smear_deficit",
            "tolerance_class": "transient_integrity.v1",
            "payload": {"kind": "scalar", "mean_deficit": round(mean_deficit, 4),
                        "worst_deficit": round(worst_deficit, 4), "onsets_detected": len(ref_onsets),
                        "cand_onsets_detected": len(cand_onsets), "onsets_scored": scored,
                        "onsets_lost": lost, "onsets_skipped": skipped,
                        "coverage": round(len(deficits) / len(ref_onsets), 3), "direction": direction}}


def _transient_integrity_summary(verdict: str, env: dict[str, Any]) -> str:
    head = _generic_head(verdict, env)
    if head:
        return head
    p = env["payload"]
    judged = p["onsets_scored"] + p["onsets_lost"]
    lost_note = f", {p['onsets_lost']} dropped" if p["onsets_lost"] else ""
    m = (f"mean attack-smear deficit {p['mean_deficit']:.2f} (worst {p['worst_deficit']:.2f}) across "
         f"{judged}/{p['onsets_detected']} onsets{lost_note}")
    if verdict == VERDICT_NO_CHANGE:
        return f"No material transient-integrity change ({m})."
    if verdict == VERDICT_REGRESSION:
        return f"Regression suspected: attacks are softer/smeared vs the golden reference ({m})."
    return f"Material transient-integrity change: candidate has {p['direction']} ({m})."


_TONAL_BALANCE = _Axis(
    profile="tonal-balance", axis="tonal_balance", tool="quality-lab:spectral_centroid",
    default_threshold=0.05, bad_sign=-1, kernel=_centroid_kernel, summarize=_tonal_summary,
)
_ADDED_HF = _Axis(
    profile="added-hf", axis="added_hf", tool="quality-lab:hf_fizz",
    default_threshold=3.0, bad_sign=+1, kernel=_added_hf_kernel, summarize=_added_hf_summary,
    threshold_range=(0.0, 60.0),  # dB magnitude — its own unit, so the shared guard travels with it
)
_NOISE_ROUGHNESS = _Axis(
    profile="noise-roughness", axis="noise_roughness", tool="quality-lab:hnr",
    default_threshold=1.5, bad_sign=-1, kernel=_noise_roughness_kernel, summarize=_noise_roughness_summary,
    threshold_range=(0.0, 60.0),  # dB magnitude (HNR delta)
)
_GRAININESS = _Axis(
    profile="graininess", axis="graininess", tool="quality-lab:spectral_flux",
    default_threshold=0.5, bad_sign=+1, kernel=_graininess_kernel, summarize=_graininess_summary,
    threshold_range=(0.0, 100.0),  # relative flux increase — can exceed 1.0 (e.g. +500%)
)
_STEREO_WIDTH = _Axis(
    profile="stereo-width", axis="stereo_width", tool="quality-lab:stereo_width",
    default_threshold=_STEREO_WIDTH_THRESHOLD, bad_sign=-1,
    kernel=_stereo_width_kernel, summarize=_stereo_width_summary,
    threshold_range=(0.0, 10.0),   # RMS(side)/RMS(mid) delta — a phase flip can push it well past 1
    needs_stereo=True,
)
_TRANSIENT_INTEGRITY = _Axis(
    profile="transient-integrity", axis="transient_integrity", tool="quality-lab:transient_sharpness",
    default_threshold=0.15, bad_sign=+1, kernel=_transient_integrity_kernel,
    summarize=_transient_integrity_summary,
    threshold_range=(0.0, 1.0),   # mean attack-smear deficit in [0,1] (0 = faithful, 1 = fully soft)
    needs_onsets=True,
)
_AXES: dict[str, _Axis] = {a.profile: a for a in
                           (_TONAL_BALANCE, _ADDED_HF, _NOISE_ROUGHNESS, _GRAININESS,
                            _STEREO_WIDTH, _TRANSIENT_INTEGRITY)}

PROFILES = tuple(sorted(_AXES))
DEFAULT_THRESHOLD = _TONAL_BALANCE.default_threshold  # back-compat alias
# Capability groupings. `stereo-width` needs 2 channels; `transient-integrity` needs onset-bearing
# (percussive) material. MONO_PROFILES = anything not needing stereo. NET_DEFAULT_PROFILES is the
# regression net's default: the broadly-applicable axes, EXCLUDING the capability-specific stereo +
# onset axes (which would emit spurious not_applicable rows on the wrong material). Those are opt-in
# per the suite's material.
STEREO_PROFILES = tuple(sorted(p for p, a in _AXES.items() if a.needs_stereo))
ONSET_PROFILES = tuple(sorted(p for p, a in _AXES.items() if a.needs_onsets))
# NOTE: MONO_PROFILES means "does not need 2 channels" — it INCLUDES the onset axis
# (transient-integrity), which is material-specific. A caller wanting broadly-applicable axes should
# use NET_DEFAULT_PROFILES, not MONO_PROFILES, to avoid not_applicable rows on the wrong material.
MONO_PROFILES = tuple(p for p in PROFILES if p not in set(STEREO_PROFILES))
NET_DEFAULT_PROFILES = tuple(p for p, a in sorted(_AXES.items())
                             if not a.needs_stereo and not a.needs_onsets)


# ── Shared measurement + verdict machinery ──────────────────────────────────────────────

def _measurement(axis: _Axis, status: str, *, applicable: bool, reason: str | None = None, **evidence: Any) -> dict[str, Any]:
    return schema.compare_measurement(
        axis.axis, axis.tool, status, applicable=applicable,
        alignment=schema.compare_alignment_not_required(), reason=reason, **evidence
    )


def _level_match_evidence(ref_rms: float, cand_rms_pre: float) -> dict[str, Any]:
    return {
        "applied": True,
        "gain_db": round(20.0 * np.log10(ref_rms / cand_rms_pre), 3) if cand_rms_pre > 1e-12 else None,
        "ref_rms_db": round(20.0 * np.log10(ref_rms), 3),
        "cand_rms_db": round(20.0 * np.log10(cand_rms_pre), 3) if cand_rms_pre > 1e-12 else None,
    }


def _measured(axis: _Axis, k: dict[str, Any], threshold: float, level_match: dict[str, Any]) -> dict[str, Any]:
    """Assemble the MEASURED envelope from a kernel result. `exceeds` and `direction_sign` are BOTH
    derived from the RAW (unrounded) delta so neither can disagree with the presentation-rounded
    `delta` at a boundary. `direction_sign` exists because `delta` rounds to 4 places: with a
    threshold finer than that (e.g. 1e-8), a raw delta that exceeds still rounds to 0.0, whose sign
    would flip the regression direction. Shared by the mono and stereo measurement paths."""
    delta = k["delta"]
    if not np.isfinite(delta):
        # Finite audio through a well-behaved kernel yields a finite delta; guard anyway so a
        # future kernel can't smuggle a NaN past the threshold check into a false verdict.
        return _measurement(axis, STATUS_INVALID, applicable=False,
                            reason=f"{axis.axis} kernel produced a non-finite delta")
    return _measurement(
        axis, STATUS_MEASURED, applicable=True, level_match=level_match,
        materiality={"delta": round(delta, 4), "unit": k["unit"], "tolerance_class": k["tolerance_class"],
                     "threshold": threshold, "exceeds": bool(abs(delta) >= threshold),
                     "direction_sign": (1 if delta > 0 else -1 if delta < 0 else 0)},
        payload=k["payload"],
    )


def _measure(axis: _Axis, reference: np.ndarray, candidate: np.ndarray, sr: int, threshold: float,
             stereo: tuple[np.ndarray, np.ndarray] | None = None) -> dict[str, Any]:
    """Shared flow for every axis: length + silence applicability, level-match, run the axis kernel,
    assemble the evidence envelope. A `needs_stereo` axis routes to the stereo path (fed the original
    2-channel signals); every other axis uses the mono downmix. `status`/`applicable`/`materiality`
    carry the honesty — a reader never has to guess whether the number is trustworthy."""
    if axis.needs_stereo:
        return _measure_stereo(axis, stereo, sr, threshold)
    ref_rms = audio_io.rms(reference)
    if reference.size < _LTAS_N_FFT or candidate.size < _LTAS_N_FFT:
        return _measurement(axis, STATUS_NOT_APPLICABLE, applicable=False, reason="material shorter than one LTAS frame")
    if ref_rms <= 1e-9:
        return _measurement(axis, STATUS_NOT_APPLICABLE, applicable=False, reason="reference is silent")

    matched = audio_io.level_match(candidate, reference)
    cand_rms_pre = audio_io.rms(candidate)
    k = axis.kernel(matched, reference, sr)
    if not k.get("applicable"):
        return _measurement(axis, STATUS_NOT_APPLICABLE, applicable=False, reason=k.get("reason", "not applicable"))
    return _measured(axis, k, threshold, _level_match_evidence(ref_rms, cand_rms_pre))


def _measure_stereo(axis: _Axis, stereo: tuple[np.ndarray, np.ndarray] | None, sr: int,
                    threshold: float) -> dict[str, Any]:
    """Measurement path for a `needs_stereo` axis. The kernel is fed the ORIGINAL (candidate,
    reference) 2-channel signals — the metric (side/mid ratio, interchannel correlation) is
    level-invariant, so no level-match is applied. Mono input on either side (no stereo pair) is
    `not_applicable`; silence gates on the STEREO signal's energy (a pure anti-phase render has a
    silent mono downmix but real stereo content, so a mono-downmix gate would wrongly reject it)."""
    if stereo is None:
        return _measurement(axis, STATUS_NOT_APPLICABLE, applicable=False,
                            reason="stereo-width requires exactly 2-channel (stereo) input on both sides")
    ref_st, cand_st = stereo
    # Harden a direct caller (compare_files only ever passes exact-2-channel arrays): a malformed
    # (N,1)/(N,3) tuple becomes not_applicable, not a raw ValueError out of the dsp primitive.
    if not (ref_st.ndim == 2 and ref_st.shape[1] == 2 and cand_st.ndim == 2 and cand_st.shape[1] == 2):
        return _measurement(axis, STATUS_NOT_APPLICABLE, applicable=False,
                            reason="stereo-width requires exactly 2-channel (stereo) input on both sides")
    if ref_st.shape[0] < _LTAS_N_FFT or cand_st.shape[0] < _LTAS_N_FFT:
        return _measurement(axis, STATUS_NOT_APPLICABLE, applicable=False, reason="material shorter than one LTAS frame")
    if float(np.sqrt(np.mean(ref_st ** 2))) <= 1e-9:
        return _measurement(axis, STATUS_NOT_APPLICABLE, applicable=False, reason="reference is silent")
    k = axis.kernel(cand_st, ref_st, sr)
    if not k.get("applicable"):
        return _measurement(axis, STATUS_NOT_APPLICABLE, applicable=False, reason=k.get("reason", "not applicable"))
    return _measured(axis, k, threshold,
                     {"applied": False, "reason": "stereo width/correlation are level-invariant"})


def _verdict(axis: _Axis, env: dict[str, Any], reference_role: str) -> str:
    """Map an evidence envelope to an action-oriented verdict. Intent-safe: `regression_
    suspected` only when the reference is golden AND the change is in the axis's bad direction."""
    if env["status"] == STATUS_INVALID:
        return VERDICT_INVALID
    if not env.get("applicable") or env.get("coverage", 0.0) < 1.0:
        return VERDICT_INCONCLUSIVE
    mat = env["materiality"]
    # `exceeds` and `direction_sign` are BOTH computed from the raw (unrounded) delta in `_measure`,
    # so presentation-rounding of `delta` can never disagree with the verdict at the boundary —
    # neither the materiality decision nor the regression DIRECTION reads the rounded `delta`.
    if not mat["exceeds"]:
        return VERDICT_NO_CHANGE
    if reference_role == "golden" and mat["direction_sign"] == axis.bad_sign:
        return VERDICT_REGRESSION
    return VERDICT_MATERIAL


# ── Raw comparators + corroboration (advisory, off-gate — S4) ────────────────────────────

def _corroboration(primary_exceeds: bool, residual_db: float, length: dict[str, Any]) -> dict[str, Any]:
    """Materiality cross-check: does the deterministic null-residual ALSO register a material
    change, agreeing with the primary axis's `exceeds`? This is NOT a trust/confidence score —
    it only reports agreement about *whether a change exists*, under the same level-matched
    global contract. Disagreement is legitimate: the raw residual is phase-sensitive and axis-
    agnostic, so it and the axis measure genuinely different things. It never moves the verdict.

    Length is already baked into `residual_db` (the shorter signal was zero-padded), so a
    truncated/extended render with content raises the residual and can never read as identity."""
    raw_material = residual_db >= _RESIDUAL_MATERIAL_DB
    agree = primary_exceeds == raw_material
    status = CORROBORATED if agree else NOT_CORROBORATED
    length_differs = length["ref_samples"] != length["cand_samples"]
    length_clause = ""
    if length_differs:
        length_clause = (f" The candidate also differs in duration "
                         f"({length['cand_samples']} vs {length['ref_samples']} samples), which the "
                         f"residual captures (the shorter signal is zero-padded).")
    if agree and primary_exceeds:
        detail = "an independent sample-domain residual also registers a material difference"
    elif agree:
        detail = "the sample-domain residual is near the identity floor, agreeing that nothing material changed"
    elif primary_exceeds:
        detail = ("the axis flags a change the sample-domain residual does not — a marginal or "
                  "phase-only difference; treat the axis result with more caution")
    else:
        detail = ("a material sample-domain difference exists that this axis does not capture "
                  "(e.g. a delay or a duration change) — try another profile")
    note = (f"Materiality cross-check under the level-matched global contract, NOT a trust score: "
            f"{detail}. Disagreement is legitimate — the raw residual (phase-sensitive) and the "
            f"axis measure different things.{length_clause}")
    return schema.compare_corroboration(
        status, note,
        basis={"raw_comparator": _RAW_NULL_TOOL, "residual_db": round(residual_db, 2),
               "residual_material_floor_db": _RESIDUAL_MATERIAL_DB,
               # bool() is load-bearing: `_headline_flags` uses `is True`/`is False` identity checks
               # on these, which a numpy.bool_ would silently fail. Keep the wraps.
               "axis_exceeds": bool(primary_exceeds), "raw_material": bool(raw_material),
               "ref_samples": length["ref_samples"], "cand_samples": length["cand_samples"]},
    )


def _advisory_block(reference: np.ndarray, candidate: np.ndarray, primary_env: dict[str, Any]) -> dict[str, Any] | None:
    """Build the report's off-gate `advisory` namespace: the deterministic null-residual raw
    comparator plus a materiality corroboration against the primary axis. Only meaningful when
    the primary measurement succeeded (it reuses the same level-match contract). Returns None
    otherwise, so a not_applicable/invalid report carries no misleading advisory."""
    if primary_env.get("status") != STATUS_MEASURED:
        return None
    # null_residual_db does its own length-robust (common-region) level match — do NOT pre-match
    # with the full-length audio_io.level_match, which trailing silence would distort.
    nr = null_residual_db(reference, candidate)
    if not np.isfinite(nr.residual_db):
        return None  # fully silent reference — the primary path already gated this, guard anyway
    length = {"ref_samples": int(reference.size), "cand_samples": int(candidate.size)}
    raw = schema.compare_raw_comparator(
        "null_residual", _RAW_NULL_TOOL, "db_rel_reference",
        ("Sample-domain residual RMS relative to the reference (20*log10(rms(cand-ref)/rms(ref))); "
         "lower = more identical. Level-matched over the common region, with the shorter signal "
         "zero-padded so a dropped/added tail counts as residual — a truncated render cannot read "
         "as identity. `level_match_applied` is false when the overlap was too quiet to define a "
         "gain (the residual is then raw). Phase/delay-sensitive and alignment-free by contract: a "
         "measure of materiality, not audibility."),
        value=round(nr.residual_db, 2),
        detail={"ref_samples": length["ref_samples"], "cand_samples": length["cand_samples"],
                "level_match_applied": nr.level_matched},
    )
    # DC-offset diagnostic (appended AFTER null_residual so the residual stays at index 0 for the
    # corroboration cross-check and its tests). A DC offset drags the LTAS centroid down and can
    # masquerade as tonal 'dulling' — surface it, off-gate, so the headline stops misdirecting.
    dc = dc_offset_metrics(reference, candidate)
    dc_raw = schema.compare_raw_comparator(
        "dc_offset", _RAW_DC_TOOL, "mean_amplitude",
        ("Per-signal DC offset (mean sample value) and its magnitude relative to RMS. A DC "
         "component concentrates energy in LTAS bin 0, pulling the spectral centroid DOWN — a "
         "nonzero offset can read as tonal 'dulling' when nothing timbral changed. Informational "
         "only; never a verdict. `present` marks an offset large enough to bias the measurement."),
        ref_value=round(dc.ref_mean, 6), cand_value=round(dc.cand_mean, 6),
        delta=round(dc.cand_mean - dc.ref_mean, 6),
        detail={"ref_frac_of_rms": round(dc.ref_frac, 4),
                "cand_frac_of_rms": round(dc.cand_frac, 4), "present": bool(dc.present)},
    )
    exceeds = bool(primary_env["materiality"]["exceeds"])
    return schema.compare_advisory([raw, dc_raw], _corroboration(exceeds, nr.residual_db, length))


def _resolve(profile: str) -> _Axis:
    axis = _AXES.get(profile)
    if axis is None:
        raise ValueError(f"unknown profile {profile!r} (available: {', '.join(PROFILES)})")
    return axis


def _headline_flags(advisory: dict[str, Any] | None) -> list[dict[str, Any]]:
    """Promote a corroboration DISAGREEMENT to a machine-readable top-level flag (never a verdict).
    Two mirrored directions off the corroboration basis:
    - axis blind, residual material  → `uncaptured_material_difference` — the single most actionable
      line for a DSP dev (a real change the chosen axis can't see). It is ALSO the known false-alarm
      for time/pitch-variant processing (the phase-sensitive residual always disagrees there), so it
      carries `expected_for=["time_variant_processing"]` for machine suppression.
    - axis flags, residual immaterial → `axis_change_without_residual` — a marginal or phase-only
      change to weigh before acting on the axis verdict."""
    if not advisory:
        return []
    basis = (advisory.get("corroboration") or {}).get("basis") or {}
    # Identity checks (not truthiness) are deliberate: they distinguish a real False from an absent
    # key (None). They rely on the basis storing genuine Python bools — `_corroboration` wraps both
    # with bool(); do NOT drop those wraps or a numpy.bool_ would fail `is True`/`is False` and
    # silently swallow a flag.
    axis_exceeds, raw_material = basis.get("axis_exceeds"), basis.get("raw_material")
    if axis_exceeds is False and raw_material is True:
        return [schema.compare_headline_flag(
            schema.COMPARE_FLAG_UNCAPTURED_DIFF,
            "an independent sample-domain residual registers a material difference this axis does not measure",
            expected_for=["time_variant_processing"])]
    if axis_exceeds is True and raw_material is False:
        return [schema.compare_headline_flag(
            schema.COMPARE_FLAG_AXIS_ONLY,
            "the axis flags a change the sample-domain residual does not register (marginal or phase-only)")]
    return []


def _summary_with_disclosures(
    axis: _Axis, verdict: str, env: dict[str, Any], advisory: dict[str, Any] | None,
    headline_flags: list[dict[str, Any]],
) -> str:
    """The axis's own summary plus any honesty disclosures (mono downmix, DC offset, corroboration
    disagreement). These are PRESENTATION ONLY — appended after the verdict is decided, reading only
    off-gate facts (the envelope's `downmix` note, the advisory's `dc_offset` comparator, and the
    `headline_flags` derived from corroboration); they never change the verdict. This stops the
    headline from actively misdirecting or from burying the most actionable corroboration signal."""
    summary = axis.summarize(verdict, env)
    # Alignment disclosure (only when latency alignment was requested — the default not_required
    # policy stays silent). It changed WHAT was measured, so surface it plainly.
    al = env.get("alignment", {})
    if al.get("policy") == schema.COMPARE_ALIGN_FIXED_LATENCY and al.get("applied"):
        summary += (f" Aligned: trimmed a {al['lag_samples']}-sample constant lag before measuring "
                    f"(confidence {al['confidence']}).")
    elif al.get("policy") == schema.COMPARE_ALIGN_NOT_ALIGNED:
        summary += f" Alignment refused ({al.get('reason', 'low confidence')}) — measured unaligned."
    # The structured `downmix` field always discloses the fold (machine-readable); the prose clause
    # is suppressed on `invalid`, where "we downmixed to mono" reads oddly on a report that never
    # actually compared anything.
    if env.get("downmix") and verdict != VERDICT_INVALID:
        summary += " Stereo/spatial image not compared — input(s) were downmixed to mono."
    if advisory:
        dc = next((r for r in advisory.get("raw_comparators", []) if r.get("name") == "dc_offset"), None)
        if dc and dc.get("detail", {}).get("present"):
            summary += (" A DC offset is present in the input(s); it biases the LTAS toward bin 0 "
                        "and can be mistaken for a tonal change — high-pass the inputs before comparing.")
    for f in headline_flags:
        if f["flag"] == schema.COMPARE_FLAG_UNCAPTURED_DIFF:
            summary += (" An independent sample-domain residual registers a material difference this "
                        "axis does not measure (expected for time/pitch-variant processing; otherwise "
                        "try another profile).")
        elif f["flag"] == schema.COMPARE_FLAG_AXIS_ONLY:
            summary += (" The sample-domain residual does not register a material difference — the "
                        "flagged change may be marginal or phase-only; weigh the axis verdict accordingly.")
    return summary


def _apply_alignment(
    align: str, reference: np.ndarray, candidate: np.ndarray, sr: int, axis: _Axis
) -> tuple[np.ndarray, np.ndarray, dict[str, Any]]:
    """Optionally trim `reference`/`candidate` to a common time base BEFORE measuring, so a constant
    delay/offset is judged as content, not as the shift. Returns (ref, cand, alignment_record); the
    record replaces the envelope's `alignment` field so a reader sees whether — and how — the pair
    was aligned. On low confidence it REFUSES (records `not_aligned`) and returns the signals
    untouched: a wrong alignment is worse than none, and the residual will still flag the offset
    (correct, since we couldn't align)."""
    # Capability-specific axes disclose their OWN alignment story regardless of the flag (checked
    # before the generic `none` case so the record is honest, not a stale "global_ltas_metric").
    # `requested` is recorded only when latency alignment was actually asked for.
    extra = {"requested": align} if align != "none" else {}
    if axis.needs_stereo:
        # stereo width/correlation are global over the whole render → invariant to a constant delay.
        return reference, candidate, schema.compare_alignment_not_required(
            reason="stereo-width is invariant to a constant delay", **extra)
    if axis.needs_onsets:
        # transient-integrity self-aligns each onset (sub-hop cross-correlation), so a global
        # constant-lag trim is redundant.
        return reference, candidate, schema.compare_alignment_not_required(
            reason="transient-integrity aligns each onset locally", **extra)
    if align == "none":
        return reference, candidate, schema.compare_alignment_not_required()
    est = estimate_global_lag(reference, candidate, sr)   # align == "latency"
    if est.confidence < _ALIGN_CONFIDENCE_FLOOR:
        return reference, candidate, schema.compare_alignment(
            schema.COMPARE_ALIGN_NOT_ALIGNED, requested="latency", applied=False,
            confidence=round(est.confidence, 3),
            reason=(f"no reliable constant lag (confidence {est.confidence:.2f} < "
                    f"{_ALIGN_CONFIDENCE_FLOOR}) — the difference is not a pure delay"))
    ref_a, cand_a = apply_lag_trim(reference, candidate, est.lag_samples)
    return ref_a, cand_a, schema.compare_alignment(
        schema.COMPARE_ALIGN_FIXED_LATENCY, applied=True,
        lag_samples=int(est.lag_samples), confidence=round(est.confidence, 3))


def compare_arrays(
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    *,
    profile: str = "tonal-balance",
    reference_role: str = "peer",
    threshold: float | None = None,
    input_channels: tuple[int, int] | None = None,
    stereo: tuple[np.ndarray, np.ndarray] | None = None,
    align: str = "none",
) -> dict[str, Any]:
    """Compare two in-memory signals and return the full report (envelope + verdict). Pure —
    no file I/O. Raises ValueError on a caller-contract error (unknown profile/role, out-of-range
    threshold, non-positive sr); returns an `invalid` report for *data* problems (non-finite
    samples). `threshold` defaults to the selected axis's default. `input_channels` is the
    ORIGINAL (ref, cand) channel counts when the signals came from multichannel files — supplied
    by `compare_files` so the report can disclose that a stereo/spatial input was folded to mono.
    `reference`/`candidate` are always mono here; `stereo` optionally carries the ORIGINAL
    (ref, cand) 2-channel arrays that a `needs_stereo` axis (stereo-width) measures — None (mono
    input) makes such an axis not_applicable."""
    axis = _resolve(profile)
    if reference_role not in ("peer", "golden"):
        raise ValueError(f"unknown reference_role {reference_role!r} (expected 'peer' or 'golden')")
    if align not in _ALIGN_MODES:
        raise ValueError(f"unknown align {align!r} (expected one of {_ALIGN_MODES})")
    if threshold is None:
        threshold = axis.default_threshold
    lo, hi = axis.threshold_range
    if not lo < threshold < hi:
        raise ValueError(f"threshold for profile {profile!r} must be in ({lo}, {hi}), got {threshold}")
    if sr <= 0:
        raise ValueError(f"sample rate must be positive, got {sr}")
    reference = np.asarray(reference, dtype=np.float64)
    candidate = np.asarray(candidate, dtype=np.float64)

    if not (bool(np.all(np.isfinite(reference))) and bool(np.all(np.isfinite(candidate)))):
        env = _measurement(axis, STATUS_INVALID, applicable=False, reason="non-finite samples (NaN/Inf)")
        advisory = None
    else:
        # Optionally align to a common time base FIRST, then measure the aligned pair. The alignment
        # record is disclosed on the envelope; on `none` it is the default not_required policy.
        aref, acand, alignment = _apply_alignment(align, reference, candidate, sr, axis)
        env = _measure(axis, aref, acand, sr, threshold, stereo=stereo)
        env["alignment"] = alignment
        # A needs_stereo axis measures the stereo image; the null-residual + corroboration run on the
        # MONO downmix, a different domain that cannot cross-check it — a pure widener (mid unchanged)
        # makes the mono residual read "identity" and would wrongly flag the real stereo change as
        # phase-only. Suppress the mono advisory rather than emit a misleading cross-check (a
        # stereo-domain residual is future work).
        # Suppress the global sample-domain advisory for axes measuring a DIFFERENT domain than the
        # mono null-residual can cross-check: the stereo image, or per-onset attacks (the onset axis
        # self-aligns, so the un-onset-aligned residual would false-disagree).
        advisory = None if (axis.needs_stereo or axis.needs_onsets) else _advisory_block(aref, acand, env)

    # Disclose the mono fold — UNLESS this axis actually measured the stereo image (stereo-width),
    # where "stereo not compared" would be false.
    if (input_channels is not None and (input_channels[0] > 1 or input_channels[1] > 1)
            and not axis.needs_stereo):
        env["downmix"] = schema.compare_downmix_note(input_channels[0], input_channels[1])

    verdict = _verdict(axis, env, reference_role)
    headline_flags = _headline_flags(advisory)
    summary = _summary_with_disclosures(axis, verdict, env, advisory, headline_flags)
    return schema.compare_report(profile, reference_role, verdict, summary, [env],
                                 advisory=advisory, headline_flags=headline_flags)


def _sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def compare_files(
    reference_wav: str,
    candidate_wav: str,
    *,
    profile: str = "tonal-balance",
    reference_role: str = "peer",
    threshold: float | None = None,
    align: str = "none",
) -> dict[str, Any]:
    """Load two WAVs and compare them. Adds provenance (hashes, sample rate). Caller-contract
    errors (unknown profile / reference_role) raise ValueError up front — same as
    `compare_arrays`, and BEFORE any file I/O so a decode failure can't mask them. *Data*
    problems (decode error, sample-rate mismatch) return an `invalid` report (never raise) with
    provenance so an agent can always trace what it read."""
    _resolve(profile)  # unknown profile → ValueError (not a silent tonal-balance fallback)
    if reference_role not in ("peer", "golden"):
        raise ValueError(f"unknown reference_role {reference_role!r} (expected 'peer' or 'golden')")
    if align not in _ALIGN_MODES:
        raise ValueError(f"unknown align {align!r} (expected one of {_ALIGN_MODES})")
    try:
        ref_raw, sr_ref, ref_ch = audio_io.load_wav_multichannel(reference_wav)
        cand_raw, sr_cand, cand_ch = audio_io.load_wav_multichannel(candidate_wav)
    except Exception as exc:  # decode/open failure → structured invalid, not a crash
        return _invalid_report(profile, reference_role, f"could not read inputs: {exc}",
                               provenance={"reference": reference_wav, "candidate": candidate_wav,
                                           "error": "decode_error"})
    if sr_ref != sr_cand:
        return _invalid_report(
            profile, reference_role, f"sample-rate mismatch: {sr_ref} vs {sr_cand} Hz",
            provenance={"reference": reference_wav, "candidate": candidate_wav,
                        "error": "sample_rate_mismatch", "ref_sample_rate": sr_ref,
                        "cand_sample_rate": sr_cand})

    # Mono downmix (mean, matching load_wav_info) is what every axis but stereo-width measures, plus
    # the advisory + length/silence gates. The original 2-channel arrays feed the stereo-width axis,
    # but only when BOTH inputs are exactly stereo — otherwise that axis is not_applicable.
    ref = ref_raw if ref_raw.ndim == 1 else ref_raw.mean(axis=1)
    cand = cand_raw if cand_raw.ndim == 1 else cand_raw.mean(axis=1)
    stereo = (ref_raw, cand_raw) if (ref_ch == 2 and cand_ch == 2) else None

    report = compare_arrays(
        ref, cand, sr_ref, profile=profile, reference_role=reference_role, threshold=threshold,
        input_channels=(ref_ch, cand_ch), stereo=stereo, align=align,
    )
    report["provenance"] = {
        "reference": reference_wav,
        "candidate": candidate_wav,
        "ref_sha256": _sha256(reference_wav),
        "cand_sha256": _sha256(candidate_wav),
        "sample_rate": sr_ref,
        "ref_channels": ref_ch,
        "cand_channels": cand_ch,
        "profile": profile,
    }
    return report


def invalid_report(
    profile: str, reference_role: str, reason: str, *, provenance: dict[str, Any] | None = None
) -> dict[str, Any]:
    """Public builder for a caller-contract `invalid` report (e.g. an out-of-range `--threshold`
    caught at a CLI/MCP surface). Emitting a structured report — instead of letting the ValueError
    escape as a traceback — lets a delegating caller (the MCP tool, which reads the report JSON)
    see a clean `invalid` verdict rather than misread an empty file as 'tool not installed'."""
    return _invalid_report(profile, reference_role, reason, provenance=provenance)


def _invalid_report(
    profile: str, reference_role: str, reason: str, *, provenance: dict[str, Any] | None = None
) -> dict[str, Any]:
    axis = _AXES.get(profile, _TONAL_BALANCE)  # identity only; profile is echoed in the report
    env = _measurement(axis, STATUS_INVALID, applicable=False, reason=reason)
    return schema.compare_report(
        profile, reference_role, VERDICT_INVALID, axis.summarize(VERDICT_INVALID, env), [env],
        provenance=provenance,
    )
