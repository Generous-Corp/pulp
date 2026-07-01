"""Agent-facing before/after comparison report (advisory, off-gate).

This is the vertical `compare` surface: given a reference and a candidate render, it
level-matches, runs ONE curated measurement (tonal-balance via the global LTAS spectral
centroid), and emits a typed *evidence envelope* plus a defended, action-oriented verdict.
It exists so an agent tuning DSP can measure a change, compare before/after, and weigh in
with cited evidence — not a bare pass/fail.

Boundaries (see planning/2026-07-01-agent-audio-measurement-substrate.md and the audio-harness
skill for the full rationale):
- **Advisory, never a gate.** The verdict is a judgment for a human/agent to act on; it does
  not move any regression gate. `pulp audio validate compare` remains the gate primitive.
- **Schema is owned by `schema.py`.** This module owns the measurement *logic*; the envelope /
  report *shape* (and the single envelope constructor) live in `schema.py` like every lab schema.
- **One axis in this slice.** Tonal-balance/centroid is a global LTAS metric — alignment-free
  (we record the policy, we don't build alignment) and scale-invariant (level-match is applied
  and recorded for honesty/consistency, though it does not move the centroid).
- **Intent-safe verdicts.** `regression_suspected` is only emitted when the caller declares the
  reference known-good (`reference_role="golden"`); a `peer` comparison of a duller candidate is
  the neutral `material_change_detected` — we do not assume which side is "right".
- Doctor THD/response and onset-drift are intentionally NOT here (they need a controlled stimulus
  / are experimental); more axes arrive behind explicit `--profile`s in a later slice.
"""
from __future__ import annotations

import hashlib
from typing import Any

import numpy as np

from . import audio_io, schema
from .dsp import relative_centroid_shift

# Re-export the schema's compare vocabulary under this module so the agent-facing API is one
# import (`from quality_lab import compare; compare.VERDICT_REGRESSION`).
SCHEMA = schema.COMPARE_SCHEMA
VERDICT_REGRESSION = schema.COMPARE_VERDICT_REGRESSION
VERDICT_MATERIAL = schema.COMPARE_VERDICT_MATERIAL
VERDICT_NO_CHANGE = schema.COMPARE_VERDICT_NO_CHANGE
VERDICT_INCONCLUSIVE = schema.COMPARE_VERDICT_INCONCLUSIVE
VERDICT_INVALID = schema.COMPARE_VERDICT_INVALID
STATUS_MEASURED = schema.COMPARE_STATUS_MEASURED
STATUS_NOT_APPLICABLE = schema.COMPARE_STATUS_NOT_APPLICABLE
STATUS_INVALID = schema.COMPARE_STATUS_INVALID

DEFAULT_THRESHOLD = 0.05  # relative centroid shift; matches the spectral_centroid detector
_LTAS_N_FFT = 2048  # a valid LTAS needs at least this many samples

# The single axis this slice measures. Kept as constants + a per-axis envelope wrapper so the
# (future) multi-axis refactor has one obvious place to generalize.
_AXIS = "tonal_balance"
_TOOL = "quality-lab:spectral_centroid"
_ALIGNMENT = {"policy": "not_required", "reason": "global_ltas_metric"}


def _finite(y: np.ndarray) -> bool:
    return bool(np.all(np.isfinite(y)))


def _tonal_measurement(status: str, *, applicable: bool, reason: str | None = None, **evidence: Any) -> dict[str, Any]:
    """Tonal-balance adapter over the schema's envelope constructor — fills this axis's
    identity + alignment policy. (S2 axes each add a sibling wrapper like this.)"""
    return schema.compare_measurement(
        _AXIS, _TOOL, status, applicable=applicable, alignment=_ALIGNMENT, reason=reason, **evidence
    )


def _tonal_balance_envelope(
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    *,
    threshold: float = DEFAULT_THRESHOLD,
) -> dict[str, Any]:
    """The tonal-balance (LTAS spectral-centroid) evidence envelope for one ref/cand pair.

    Level-matches the candidate first (rule #1), then compares the global LTAS centroids via
    the shared `relative_centroid_shift` (same primitive the detector uses, so they can't
    drift). `status`/`applicable`/`materiality` carry the honesty — a reader never has to
    guess whether the number is trustworthy."""
    ref_rms = audio_io.rms(reference)
    # Applicability: need enough material for an LTAS and a non-silent, broadband reference.
    if reference.size < _LTAS_N_FFT or candidate.size < _LTAS_N_FFT:
        return _tonal_measurement(STATUS_NOT_APPLICABLE, applicable=False,
                                  reason="material shorter than one LTAS frame")
    if ref_rms <= 1e-9:
        return _tonal_measurement(STATUS_NOT_APPLICABLE, applicable=False, reason="reference is silent")

    matched = audio_io.level_match(candidate, reference)
    cand_rms_pre = audio_io.rms(candidate)
    rel, c_ref, c_cand = relative_centroid_shift(reference, matched, sr)
    if c_ref <= 1e-9:
        return _tonal_measurement(STATUS_NOT_APPLICABLE, applicable=False,
                                  reason="reference centroid undefined (no spectral energy)")

    direction = "duller" if rel < 0 else "brighter"
    return _tonal_measurement(
        STATUS_MEASURED,
        applicable=True,
        level_match={
            "applied": True,
            "gain_db": round(20.0 * np.log10(ref_rms / cand_rms_pre), 3) if cand_rms_pre > 1e-12 else None,
            "ref_rms_db": round(20.0 * np.log10(ref_rms), 3),
            "cand_rms_db": round(20.0 * np.log10(cand_rms_pre), 3) if cand_rms_pre > 1e-12 else None,
        },
        materiality={
            "delta": round(rel, 4),
            "unit": "rel_centroid_shift",
            "tolerance_class": "spectral_centroid.v1",
            "threshold": threshold,
            "exceeds": bool(abs(rel) >= threshold),
        },
        payload={
            "kind": "scalar",
            "ref_centroid_hz": round(c_ref, 1),
            "cand_centroid_hz": round(c_cand, 1),
            "rel_shift": round(rel, 4),
            "direction": direction,
        },
    )


def _tonal_balance_verdict_for(envelope: dict[str, Any], *, reference_role: str) -> str:
    """Map one tonal-balance envelope to an action-oriented verdict. Intent-safe: `regression_
    suspected` only when the reference is declared golden. (Named per-axis: it assumes a signed
    centroid shift where negative = duller, so it must NOT be reused for other axes.)"""
    if envelope["status"] == STATUS_INVALID:
        return VERDICT_INVALID
    if not envelope.get("applicable") or envelope.get("coverage", 0.0) < 1.0:
        return VERDICT_INCONCLUSIVE
    rel = envelope["materiality"]["delta"]
    threshold = envelope["materiality"]["threshold"]
    if abs(rel) < threshold:
        return VERDICT_NO_CHANGE
    if reference_role == "golden" and rel < -threshold:  # candidate is duller than known-good
        return VERDICT_REGRESSION
    return VERDICT_MATERIAL


def _summary(verdict: str, env: dict[str, Any]) -> str:
    if verdict == VERDICT_INVALID:
        return f"Measurement invalid: {env.get('reason', 'could not analyze inputs')}."
    if verdict == VERDICT_INCONCLUSIVE:
        return f"Inconclusive: {env.get('reason', 'measurement could not support a verdict')}."
    p = env["payload"]
    shift = (f"LTAS centroid {p['ref_centroid_hz']:.0f}->{p['cand_centroid_hz']:.0f} Hz "
             f"({p['direction']} {abs(p['rel_shift'])*100:.1f}%)")
    if verdict == VERDICT_NO_CHANGE:
        return f"No material tonal-balance change ({shift})."
    if verdict == VERDICT_REGRESSION:
        return f"Regression suspected: candidate is materially duller than the golden reference ({shift})."
    return f"Material tonal-balance change: candidate is {p['direction']} ({shift})."


def compare_arrays(
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    *,
    profile: str = "tonal-balance",
    reference_role: str = "peer",
    threshold: float = DEFAULT_THRESHOLD,
) -> dict[str, Any]:
    """Compare two in-memory signals and return the full report (envelope + verdict). Pure —
    no file I/O — so it is trivially testable. Raises ValueError on a caller-contract error
    (unknown profile/role, out-of-range threshold, non-positive sr); returns an `invalid`
    report for *data* problems (non-finite samples)."""
    if profile != "tonal-balance":
        raise ValueError(f"unknown profile {profile!r} (only 'tonal-balance' in this slice)")
    if reference_role not in ("peer", "golden"):
        raise ValueError(f"unknown reference_role {reference_role!r} (expected 'peer' or 'golden')")
    if not 0.0 < threshold < 1.0:
        raise ValueError(f"threshold must be in (0, 1), got {threshold}")
    if sr <= 0:
        raise ValueError(f"sample rate must be positive, got {sr}")
    reference = np.asarray(reference, dtype=np.float64)
    candidate = np.asarray(candidate, dtype=np.float64)

    if not (_finite(reference) and _finite(candidate)):
        env = _tonal_measurement(STATUS_INVALID, applicable=False, reason="non-finite samples (NaN/Inf)")
    else:
        env = _tonal_balance_envelope(reference, candidate, sr, threshold=threshold)

    verdict = _tonal_balance_verdict_for(env, reference_role=reference_role)
    return schema.compare_report(profile, reference_role, verdict, _summary(verdict, env), [env])


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
    threshold: float = DEFAULT_THRESHOLD,
) -> dict[str, Any]:
    """Load two WAVs and compare them. Adds provenance (hashes, sample rate). Returns an
    `invalid` report (never raises) on a decode error or sample-rate mismatch — with provenance
    so an agent can always trace what it read — instead of crashing."""
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
        "detector_version": "spectral_centroid.v1",
        "threshold": threshold,
    }
    return report


def _invalid_report(
    profile: str, reference_role: str, reason: str, *, provenance: dict[str, Any] | None = None
) -> dict[str, Any]:
    env = _tonal_measurement(STATUS_INVALID, applicable=False, reason=reason)
    return schema.compare_report(
        profile, reference_role, VERDICT_INVALID, _summary(VERDICT_INVALID, env), [env],
        provenance=provenance,
    )
