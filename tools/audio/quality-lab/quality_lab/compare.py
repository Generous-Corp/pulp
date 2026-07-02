"""Agent-facing before/after comparison report (advisory, off-gate).

The vertical `compare` surface: given a reference and a candidate render, it level-matches,
runs one curated **axis** (selected by `--profile`), and emits a typed *evidence envelope*
plus a defended, action-oriented verdict. It exists so an agent tuning DSP can measure a
change, compare before/after, and weigh in with cited evidence — not a bare pass/fail.

Axes are registered in `_AXES`: each is a global, alignment-free, reference-vs-candidate
metric that mirrors the same contract, so adding one is a single registry entry (the shared
`_measure`/`_verdict` machinery does level-matching, applicability, materiality, and the
intent-safe verdict). Today: `tonal-balance` (LTAS spectral-centroid shift) and `added-hf`
(high-frequency fizz fraction). Axes that need a controlled stimulus (THD/response) or
alignment (transients/timing) are deliberately NOT here.

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
from .dsp import hf_fraction_delta, null_residual_db, relative_centroid_shift

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

_LTAS_N_FFT = 2048  # a valid LTAS needs at least this many samples
_ALIGNMENT = {"policy": "not_required", "reason": "global_ltas_metric"}

# Above this level-matched, reference-relative residual, an algorithm-agnostic sample-domain
# difference is present (the two renders are not effectively identical). Heuristic floor: -60 dB
# rel means the residual is ~1/1000 of the signal RMS. It gates *materiality* only, never the
# verdict — see `_corroboration`. A length mismatch needs NO separate threshold: null_residual_db
# zero-pads the shorter signal, so a dropped/added tail with content raises the residual on its
# own (and trailing silence does not), keeping the identity claim honest for any render length.
_RESIDUAL_MATERIAL_DB = -60.0
_RAW_NULL_TOOL = "quality-lab:null_residual"


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
    delta, hf_ref, hf_cand = hf_fraction_delta(reference, matched, sr)
    direction = "added HF" if delta > 0 else "reduced HF"
    return {"applicable": True, "delta": delta, "unit": "hf_energy_frac_delta",
            "tolerance_class": "hf_fizz.v1",
            "payload": {"kind": "scalar", "ref_hf_frac": round(hf_ref, 4),
                        "cand_hf_frac": round(hf_cand, 4), "hf_frac_delta": round(delta, 4),
                        "direction": direction}}


def _added_hf_summary(verdict: str, env: dict[str, Any]) -> str:
    head = _generic_head(verdict, env)
    if head:
        return head
    p = env["payload"]
    frac = (f"HF(>=8kHz) fraction {p['ref_hf_frac']:.3f}->{p['cand_hf_frac']:.3f} "
            f"(delta {p['hf_frac_delta']:+.3f})")
    if verdict == VERDICT_NO_CHANGE:
        return f"No material high-frequency change ({frac})."
    if verdict == VERDICT_REGRESSION:
        return f"Regression suspected: candidate added metallic high-frequency fizz vs the golden reference ({frac})."
    return f"Material high-frequency change: candidate {p['direction']} ({frac})."


_TONAL_BALANCE = _Axis(
    profile="tonal-balance", axis="tonal_balance", tool="quality-lab:spectral_centroid",
    default_threshold=0.05, bad_sign=-1, kernel=_centroid_kernel, summarize=_tonal_summary,
)
_ADDED_HF = _Axis(
    profile="added-hf", axis="added_hf", tool="quality-lab:hf_fizz",
    default_threshold=0.02, bad_sign=+1, kernel=_added_hf_kernel, summarize=_added_hf_summary,
)
_AXES: dict[str, _Axis] = {a.profile: a for a in (_TONAL_BALANCE, _ADDED_HF)}

PROFILES = tuple(sorted(_AXES))
DEFAULT_THRESHOLD = _TONAL_BALANCE.default_threshold  # back-compat alias


# ── Shared measurement + verdict machinery ──────────────────────────────────────────────

def _measurement(axis: _Axis, status: str, *, applicable: bool, reason: str | None = None, **evidence: Any) -> dict[str, Any]:
    return schema.compare_measurement(
        axis.axis, axis.tool, status, applicable=applicable, alignment=_ALIGNMENT, reason=reason, **evidence
    )


def _level_match_evidence(ref_rms: float, cand_rms_pre: float) -> dict[str, Any]:
    return {
        "applied": True,
        "gain_db": round(20.0 * np.log10(ref_rms / cand_rms_pre), 3) if cand_rms_pre > 1e-12 else None,
        "ref_rms_db": round(20.0 * np.log10(ref_rms), 3),
        "cand_rms_db": round(20.0 * np.log10(cand_rms_pre), 3) if cand_rms_pre > 1e-12 else None,
    }


def _measure(axis: _Axis, reference: np.ndarray, candidate: np.ndarray, sr: int, threshold: float) -> dict[str, Any]:
    """Shared flow for every global axis: length + silence applicability, level-match, run the
    axis kernel, assemble the evidence envelope. `status`/`applicable`/`materiality` carry the
    honesty — a reader never has to guess whether the number is trustworthy."""
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

    delta = k["delta"]
    if not np.isfinite(delta):
        # Finite audio through a well-behaved kernel yields a finite delta; guard anyway so a
        # future kernel can't smuggle a NaN past the threshold check into a false verdict.
        return _measurement(axis, STATUS_INVALID, applicable=False,
                            reason=f"{axis.axis} kernel produced a non-finite delta")
    return _measurement(
        axis, STATUS_MEASURED, applicable=True,
        level_match=_level_match_evidence(ref_rms, cand_rms_pre),
        materiality={"delta": round(delta, 4), "unit": k["unit"], "tolerance_class": k["tolerance_class"],
                     "threshold": threshold, "exceeds": bool(abs(delta) >= threshold)},
        payload=k["payload"],
    )


def _verdict(axis: _Axis, env: dict[str, Any], reference_role: str) -> str:
    """Map an evidence envelope to an action-oriented verdict. Intent-safe: `regression_
    suspected` only when the reference is golden AND the change is in the axis's bad direction."""
    if env["status"] == STATUS_INVALID:
        return VERDICT_INVALID
    if not env.get("applicable") or env.get("coverage", 0.0) < 1.0:
        return VERDICT_INCONCLUSIVE
    mat = env["materiality"]
    # `exceeds` is the ONE materiality decision, computed from the raw (unrounded) delta in
    # `_measure`. Keying the verdict off it means presentation-rounding of `delta` can never
    # disagree with the envelope's own `exceeds` flag at the threshold boundary.
    if not mat["exceeds"]:
        return VERDICT_NO_CHANGE
    # `exceeds` implies |raw delta| >= threshold (> 0), so the rounded delta preserves the sign.
    if reference_role == "golden" and (1 if mat["delta"] > 0 else -1) == axis.bad_sign:
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
    exceeds = bool(primary_env["materiality"]["exceeds"])
    return schema.compare_advisory([raw], _corroboration(exceeds, nr.residual_db, length))


def _resolve(profile: str) -> _Axis:
    axis = _AXES.get(profile)
    if axis is None:
        raise ValueError(f"unknown profile {profile!r} (available: {', '.join(PROFILES)})")
    return axis


def compare_arrays(
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    *,
    profile: str = "tonal-balance",
    reference_role: str = "peer",
    threshold: float | None = None,
) -> dict[str, Any]:
    """Compare two in-memory signals and return the full report (envelope + verdict). Pure —
    no file I/O. Raises ValueError on a caller-contract error (unknown profile/role, out-of-range
    threshold, non-positive sr); returns an `invalid` report for *data* problems (non-finite
    samples). `threshold` defaults to the selected axis's default."""
    axis = _resolve(profile)
    if reference_role not in ("peer", "golden"):
        raise ValueError(f"unknown reference_role {reference_role!r} (expected 'peer' or 'golden')")
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
        env = _measure(axis, reference, candidate, sr, threshold)
        advisory = _advisory_block(reference, candidate, env)

    verdict = _verdict(axis, env, reference_role)
    return schema.compare_report(
        profile, reference_role, verdict, axis.summarize(verdict, env), [env], advisory=advisory
    )


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
) -> dict[str, Any]:
    """Load two WAVs and compare them. Adds provenance (hashes, sample rate). Caller-contract
    errors (unknown profile / reference_role) raise ValueError up front — same as
    `compare_arrays`, and BEFORE any file I/O so a decode failure can't mask them. *Data*
    problems (decode error, sample-rate mismatch) return an `invalid` report (never raise) with
    provenance so an agent can always trace what it read."""
    _resolve(profile)  # unknown profile → ValueError (not a silent tonal-balance fallback)
    if reference_role not in ("peer", "golden"):
        raise ValueError(f"unknown reference_role {reference_role!r} (expected 'peer' or 'golden')")
    try:
        ref, sr_ref = audio_io.load_wav(reference_wav)
        cand, sr_cand = audio_io.load_wav(candidate_wav)
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

    report = compare_arrays(
        ref, cand, sr_ref, profile=profile, reference_role=reference_role, threshold=threshold
    )
    report["provenance"] = {
        "reference": reference_wav,
        "candidate": candidate_wav,
        "ref_sha256": _sha256(reference_wav),
        "cand_sha256": _sha256(candidate_wav),
        "sample_rate": sr_ref,
        "profile": profile,
    }
    return report


def _invalid_report(
    profile: str, reference_role: str, reason: str, *, provenance: dict[str, Any] | None = None
) -> dict[str, Any]:
    axis = _AXES.get(profile, _TONAL_BALANCE)  # identity only; profile is echoed in the report
    env = _measurement(axis, STATUS_INVALID, applicable=False, reason=reason)
    return schema.compare_report(
        profile, reference_role, VERDICT_INVALID, axis.summarize(VERDICT_INVALID, env), [env],
        provenance=provenance,
    )
