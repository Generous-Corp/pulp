"""Measurement-time alignment for `compare`: bring reference + candidate to a common time base
BEFORE any axis measures them, so a constant delay (or, in later Tier 3 slices, a declared time-warp)
is judged as content, not as the shift.

This module owns the `--align <mode>[:param]` grammar (:func:`parse`) and the per-mode dispatch
(:func:`apply`). The alignment RECORD shape is owned by ``schema.py``
(``compare_alignment`` / ``COMPARE_ALIGN_*``) — this module only constructs records, never redefines
their vocabulary.

Import contract (no cycle): this module imports ``schema`` + ``dsp`` (+ ``align`` for onset-based
warps in later slices) — never ``compare``. Axis capability facts arrive as the ``needs_stereo`` /
``needs_onsets`` booleans, never as an ``_Axis``. ``compare.py`` imports THIS module.

Not to be confused with ``align.py``, which holds the onset-detection/matching primitives that this
module's onset-anchored warp handlers will *consume*.
"""
from __future__ import annotations

import math
from typing import Any, Callable, NamedTuple

import numpy as np

from . import schema
from .align import detect_onsets
from .dsp import apply_lag_trim, estimate_global_lag, resample_to_length

# Below this cross-correlation confidence a constant-lag trim is REFUSED (a wrong alignment is worse
# than none) and recorded as `not_aligned`. Floor 0.7 sits above spurious partial/out-of-range
# matches (~0.68) yet well below real delays (~0.9-1.0); ambiguous (periodic) and boundary peaks are
# already forced to confidence 0 in the estimator.
_ALIGN_CONFIDENCE_FLOOR = 0.7
# A declared `varispeed:R` is VERIFIED against the observed duration ratio before it is trusted: the
# candidate must actually be ~R× the reference's length. 0.5% is tight enough to reject a materially
# wrong declaration (a ~2% speed/pitch error) yet absorbs sample-count rounding; outside it the audio
# does not support the declaration, so we REFUSE (a wrong warp is worse than none) — §6.1 sanity. The
# resample itself is length-based (to reference.size), so the record carries the OBSERVED ratio too.
_VARISPEED_RATIO_TOL = 0.005
# Declared-warp parameter domain: beyond this the §5 measurement assumptions degrade, so we refuse an
# honest "outside supported warp range" rather than return unvalidated numbers.
_WARP_RATIO_MIN, _WARP_RATIO_MAX = 0.25, 4.0
# `stretch:R` duration sanity (§6.1): generous (renders carry latency pad + tail ring-out).
_STRETCH_DUR_TOL = 0.03
# §6.3 post-map verification — a single uniform ratio must fit onset-bearing material, else it is
# non-uniformly warped and one declared ratio is wrong. Per matched onset, the residual of the
# nearest actual candidate onset from the uniform prediction (ref_t·R) must have small MAD: a uniform
# warp clusters near 0 ms (empirically ≤ ~3 ms across R∈[0.25,4]), a non-uniform one scatters (≥ ~60
# ms). Fewer than this many onsets on either side → sustained material, which has no landmarks to
# verify and whose time-average axes are unharmed by non-uniformity → accept as unverified.
_ONSET_LAG_MAD_MS = 15.0
_STRETCH_MIN_ONSETS = 4


class AlignSpec(NamedTuple):
    """A parsed `--align` request: the raw string (echoed into the record's `requested`), the mode,
    and the coerced parameter (None for `none`/`latency`)."""
    raw: str
    mode: str
    param: float | str | None


# Handler signature: (reference, candidate, sr, param, extra) -> (ref, cand, alignment_record).
AlignHandler = Callable[
    [np.ndarray, np.ndarray, int, Any, dict[str, Any]],
    tuple[np.ndarray, np.ndarray, dict[str, Any]],
]


# ── grammar ──────────────────────────────────────────────────────────────────────────────────────
def parse(align: str) -> AlignSpec:
    """Validate + parse a `--align mode[:param]` string into an :class:`AlignSpec`. Raises
    ``ValueError`` on an unknown mode (the same exception the call sites raised for the old
    ``choices``). Called at the TOP of ``compare_arrays``/``compare_files`` — before any file I/O — so
    a caller-contract error surfaces before a decode."""
    mode, sep, param_str = align.partition(":")
    if mode not in _HANDLERS:
        raise ValueError(f"unknown align {align!r} (expected one of {tuple(_HANDLERS)})")
    if mode in ("none", "latency"):
        # A bare `none`/`latency` only — a trailing colon (even empty) is malformed, matching the old
        # exact-string contract these modes shipped with.
        if sep:
            raise ValueError(f"align {mode!r} takes no parameter (got {align!r})")
        return AlignSpec(raw=align, mode=mode, param=None)
    if mode in ("varispeed", "stretch"):
        if not param_str:
            raise ValueError(f"align {mode!r} needs a ratio, e.g. {mode}:1.5")
        try:
            ratio = float(param_str)
        except ValueError:
            raise ValueError(f"{mode} ratio must be a number, got {param_str!r}")
        # Reject non-finite ratios: inf slips past `ratio > 0` AND makes the downstream verification
        # gate `abs(observed - ratio)/ratio` evaluate to nan (nan > tol is False), which would silently
        # disable the refusal for every candidate. nan is already caught by `> 0` — guard both.
        if not math.isfinite(ratio) or ratio <= 0.0:
            raise ValueError(f"{mode} ratio must be a positive finite number, got {ratio}")
        return AlignSpec(raw=align, mode=mode, param=ratio)
    if mode == "pitch":
        if not param_str:
            raise ValueError("align 'pitch' needs semitones, e.g. pitch:+3 or pitch:-5st")
        raw_s = param_str.removesuffix("st").removesuffix("semitones").strip()
        try:
            semitones = float(raw_s)
        except ValueError:
            raise ValueError(f"pitch shift must be a number of semitones, got {param_str!r}")
        if not math.isfinite(semitones):
            raise ValueError(f"pitch semitones must be finite, got {semitones}")
        return AlignSpec(raw=align, mode=mode, param=semitones)
    # unreachable while every mode above has a param branch — a guard for a mis-registered handler.
    raise ValueError(f"align {mode!r} has no parameter grammar defined")


# ── per-mode handlers ──────────────────────────────────────────────────────────────────────────────
def _align_none(reference, candidate, sr, param, extra):
    # Exact record preserved for back-compat: {"policy":"not_required","reason":"global_ltas_metric"}.
    return reference, candidate, schema.compare_alignment_not_required()


def _align_latency(reference, candidate, sr, param, extra):
    """Estimate one constant lag and trim to a common time base, or REFUSE below the confidence floor
    (record `not_aligned`, signals untouched — a wrong alignment is worse than none, and the residual
    still flags the offset)."""
    est = estimate_global_lag(reference, candidate, sr)
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


def _align_varispeed(reference, candidate, sr, param, extra):
    """Undo a declared `varispeed:R` (tape-style speed change — pitch and time coupled) by resampling
    the candidate back to the reference's time base, so the ENTIRE existing alignment-free pipeline —
    including the phase-sensitive sample residual — measures the pair unchanged. A resample is EXACT
    for this class (not an approximation of it), which is why varispeed ships first.

    First VERIFY the declaration (§6.1): the candidate must actually be ~R× the reference's duration.
    If it isn't, the audio does not support the caller's stated transform, so REFUSE (record
    `not_aligned`) rather than resample to a wrong length — a wrong warp is worse than none."""
    ratio = float(param)
    requested = extra.get("requested")
    if reference.size == 0 or candidate.size == 0:
        return reference, candidate, schema.compare_alignment(
            schema.COMPARE_ALIGN_NOT_ALIGNED, requested=requested, applied=False,
            reason="cannot verify a varispeed ratio against an empty signal")
    observed = candidate.size / reference.size
    if abs(observed - ratio) / ratio > _VARISPEED_RATIO_TOL:
        return reference, candidate, schema.compare_alignment(
            schema.COMPARE_ALIGN_NOT_ALIGNED, requested=requested, applied=False,
            declared_ratio=ratio, observed_ratio=round(observed, 4),
            reason=(f"declared varispeed {ratio}× but the candidate is {observed:.3f}× the reference "
                    "duration — the audio does not support the stated transform"))
    restored = resample_to_length(candidate, reference.size)
    return reference, restored, schema.compare_alignment(
        schema.COMPARE_ALIGN_VARISPEED, requested=requested, applied=True,
        declared_ratio=ratio, observed_ratio=round(observed, 4))


def _verify_uniform_stretch(
    reference: np.ndarray, candidate: np.ndarray, sr: int, ratio: float
) -> tuple[bool, str, dict[str, Any]]:
    """§6.3 post-map verification: does a SINGLE uniform ratio actually fit, using evidence independent
    of the axes measured? On onset-bearing material, match each reference onset to the nearest ACTUAL
    candidate onset around its uniform-ratio prediction (ref_t·R) — matched independently per onset, so
    (unlike a strict monotonic 1:1 map) it does not drift when a compression's refractory folds close
    onsets together — and take the robust MAD of the residuals: a uniform warp clusters near 0, a
    non-uniform one scatters past the floor → REFUSE. Too few onsets on either side is sustained
    material with no landmarks to verify and time-average axes unharmed by non-uniformity → ACCEPT as
    unverified (refusing there would only reject valid clean stretches). Returns (ok, reason, evidence)."""
    ref_onsets = detect_onsets(reference, sr)
    cand_onsets = np.asarray(detect_onsets(candidate, sr))
    if len(ref_onsets) < _STRETCH_MIN_ONSETS or cand_onsets.size < _STRETCH_MIN_ONSETS:
        return True, "", {"uniformity": "unverified — too few onsets (sustained material)"}
    resid = np.array([float(cand_onsets[np.argmin(np.abs(cand_onsets - ref_t * ratio))]) - ref_t * ratio
                      for ref_t in ref_onsets])
    mad_ms = float(np.median(np.abs(resid - np.median(resid)))) * 1000.0
    evidence = {"onset_lag_mad_ms": round(mad_ms, 1)}
    if mad_ms > _ONSET_LAG_MAD_MS:
        return (False, f"onset lags inconsistent with a uniform ratio (MAD {mad_ms:.0f} ms) — "
                "material appears non-uniformly warped", evidence)
    return True, "", evidence


def _align_stretch(reference, candidate, sr, param, extra):
    """A declared `stretch:R` (pitch-preserving time-stretch): the axes measure the UNWARPED pair
    directly — LTAS/HNR/width are time-average and warp-invariant, so a resample would only add
    artifacts. The record carries the ratio so the graininess kernel (hop-scaled flux) and the
    corroborator (LTAS residual, phase-blind) normalize for the warp. First VERIFY: §6.1 duration
    sanity + parameter domain, then §6.3 that a single uniform ratio actually fits (else the material
    is non-uniformly warped and one ratio is wrong) — REFUSE rather than measure a bad map."""
    ratio = float(param)
    requested = extra.get("requested")
    if not (_WARP_RATIO_MIN <= ratio <= _WARP_RATIO_MAX):
        return reference, candidate, schema.compare_alignment(
            schema.COMPARE_ALIGN_NOT_ALIGNED, requested=requested, applied=False,
            reason=f"stretch ratio {ratio} outside the supported range [{_WARP_RATIO_MIN}, {_WARP_RATIO_MAX}]")
    if reference.size == 0 or candidate.size == 0:
        return reference, candidate, schema.compare_alignment(
            schema.COMPARE_ALIGN_NOT_ALIGNED, requested=requested, applied=False,
            reason="cannot verify a stretch ratio against an empty signal")
    observed = candidate.size / reference.size
    if abs(observed - ratio) / ratio > _STRETCH_DUR_TOL:
        return reference, candidate, schema.compare_alignment(
            schema.COMPARE_ALIGN_NOT_ALIGNED, requested=requested, applied=False,
            declared_ratio=ratio, observed_ratio=round(observed, 4),
            reason=(f"declared stretch {ratio}× but the candidate is {observed:.3f}× the reference "
                    "duration — the audio does not support the stated transform"))
    ok, reason, evidence = _verify_uniform_stretch(reference, candidate, sr, ratio)
    if not ok:
        return reference, candidate, schema.compare_alignment(
            schema.COMPARE_ALIGN_NOT_ALIGNED, requested=requested, applied=False,
            declared_ratio=ratio, observed_ratio=round(observed, 4), reason=reason, **evidence)
    return reference, candidate, schema.compare_alignment(
        schema.COMPARE_ALIGN_STRETCH, applied=True, requested=requested,
        declared_ratio=ratio, observed_ratio=round(observed, 4), **evidence)


_PITCH_MAX_SEMITONES = 24.0
_PITCH_DUR_TOL = 0.03   # pitch is duration-preserving: the candidate must keep the reference length


def _align_pitch(reference, candidate, sr, param, extra):
    """A declared `pitch:S` (duration-preserving pitch shift, S semitones). The time base is unchanged,
    so the axes measure the pair directly — tonal-balance compensates the centroid for the EXPECTED
    log-frequency move and the corroboration binds to the shift-compensated LTAS distance. Verify §6.1:
    |S| ≤ 24 st and the duration is preserved (a length change means it is not a pure pitch shift) —
    REFUSE otherwise."""
    semitones = float(param)
    requested = extra.get("requested")
    if semitones == 0.0:
        # A declared zero-semitone shift is a no-op — behave exactly like `none` (measure the raw pair
        # with the sample-domain corroborator), not a pitch class that would swap in the delay-blind
        # LTAS corroborator and suppress a genuine offset/material residual.
        return reference, candidate, schema.compare_alignment_not_required()
    if abs(semitones) > _PITCH_MAX_SEMITONES:
        return reference, candidate, schema.compare_alignment(
            schema.COMPARE_ALIGN_NOT_ALIGNED, requested=requested, applied=False,
            reason=f"pitch shift {semitones} st outside the supported range ±{_PITCH_MAX_SEMITONES} st")
    if reference.size == 0 or candidate.size == 0:
        return reference, candidate, schema.compare_alignment(
            schema.COMPARE_ALIGN_NOT_ALIGNED, requested=requested, applied=False,
            reason="cannot verify a pitch shift against an empty signal")
    dur_rel = abs(candidate.size - reference.size) / reference.size
    if dur_rel > _PITCH_DUR_TOL:
        return reference, candidate, schema.compare_alignment(
            schema.COMPARE_ALIGN_NOT_ALIGNED, requested=requested, applied=False,
            declared_semitones=semitones, observed_duration_ratio=round(candidate.size / reference.size, 4),
            reason=(f"declared pitch:{semitones:+g}st is duration-preserving but the candidate is "
                    f"{candidate.size / reference.size:.3f}× the reference length — not a pure pitch shift"))
    return reference, candidate, schema.compare_alignment(
        schema.COMPARE_ALIGN_PITCH, applied=True, requested=requested, declared_semitones=semitones)


# Mode → handler. A new declared-warp class (ratio) is ONE entry here plus one handler function (and,
# if it needs a new policy string, one constant in schema.py).
_HANDLERS: dict[str, AlignHandler] = {
    "none": _align_none,
    "latency": _align_latency,
    "varispeed": _align_varispeed,
    "stretch": _align_stretch,
    "pitch": _align_pitch,
}

# Modes that change the TIME BASE or SPECTRUM (a warp), not just a constant offset. The capability axes
# that skip a constant-lag alignment (stereo image, per-onset self-alignment) are NOT invariant to a
# warp, so a warp mode is still applied/routed for them. New warp classes (ratio) join this set.
_WARP_MODES = frozenset({"varispeed", "stretch", "pitch"})
# Warp modes whose axes measure the UNWARPED pair (no resample) and therefore need per-axis warp
# normalization downstream (graininess hop-scaling / tonal-balance pitch compensation + the
# LTAS-residual corroborator). `varispeed` is NOT here — it resamples back, so the standard pipeline
# applies verbatim.
_MEASURE_UNWARPED_MODES = frozenset({"stretch", "pitch"})


# ── entry point (called by compare.compare_arrays) ─────────────────────────────────────────────────
def apply(
    spec: AlignSpec,
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    *,
    needs_stereo: bool,
    needs_onsets: bool,
) -> tuple[np.ndarray, np.ndarray, dict[str, Any]]:
    """Return ``(ref, cand, alignment_record)``; the record replaces the envelope's ``alignment``
    field. Capability-specific axes disclose their OWN alignment story regardless of the requested
    mode (checked first, so the record is honest rather than a stale ``global_ltas_metric``);
    ``requested`` is echoed only when a non-``none`` mode was asked for."""
    extra = {"requested": spec.raw} if spec.mode != "none" else {}
    is_warp = spec.mode in _WARP_MODES
    if needs_stereo:
        # stereo-width is a global side/mid RATIO — invariant to a constant delay AND to a uniform
        # resample (both channels scale together), so no alignment mode changes it. (The handler
        # operates on the mono downmix anyway, not the stereo image the axis reads.)
        reason = ("stereo-width is invariant to a uniform time-warp" if is_warp
                  else "stereo-width is invariant to a constant delay")
        return reference, candidate, schema.compare_alignment_not_required(reason=reason, **extra)
    if needs_onsets and not is_warp:
        # transient-integrity self-aligns each onset (sub-hop cross-correlation), so a constant lag is
        # redundant — BUT a warp stretches the attacks THEMSELVES, which per-onset alignment cannot
        # undo, so a warp mode is NOT skipped here: it falls through and is applied before the axis
        # measures (otherwise a clean varispeed reads a false attack-smear regression).
        return reference, candidate, schema.compare_alignment_not_required(
            reason="transient-integrity aligns each onset locally", **extra)
    return _HANDLERS[spec.mode](reference, candidate, sr, spec.param, extra)


def unsupported_axis(spec: AlignSpec, profile: str) -> str | None:
    """A reason string if `profile` is NOT valid under the declared warp (so `compare` declines it as
    not_applicable rather than emit a misleading verdict), else None. Under `pitch:S` only tonal-balance
    is compensated for the shift; every other axis is pitch-VARIANT (a shift legitimately moves the HF
    fraction, the HNR pitch-lag window, and the attack high band) and would false-flag a clean shift as
    a regression. Other modes support all axes (stretch: warp-invariant time-averages, graininess
    warp-normalized; varispeed: resampled back; none/latency: no warp)."""
    if spec.mode == "pitch" and profile != "tonal-balance":
        return ("axis is not pitch-invariant — only tonal-balance is compensated under --align pitch:S; "
                "measure this axis without --align pitch (or on a same-pitch pair)")
    return None


# ── warp-normalization hook (compare.py routes graininess + tonal-balance + corroboration off this) ──
def measures_unwarped(spec: AlignSpec) -> bool:
    """True when the axes measure the UNWARPED pair (a `stretch` or `pitch` class): the phase- and
    time-sensitive sample residual is invalid for corroboration and is replaced by the phase-blind LTAS
    distance, and the warp-aware axes normalize for the declared class (graininess hop-scales under
    stretch, tonal-balance pitch-compensates under pitch). False for none/latency/varispeed (varispeed
    resamples back, so the standard pipeline applies verbatim). compare.py reads the class + parameter
    off the `AlignSpec` directly for the per-axis normalization."""
    return spec.mode in _MEASURE_UNWARPED_MODES


# ── presentation (owns the alignment prose, so a new policy adds its clause in one file) ────────────
def describe(record: dict[str, Any]) -> str | None:
    """The one-line summary clause for an alignment record, or None when the record is silent (the
    default `not_required` policy). A new warp policy adds its clause here — not in compare.py."""
    policy = record.get("policy")
    if policy == schema.COMPARE_ALIGN_FIXED_LATENCY and record.get("applied"):
        return (f"Aligned: trimmed a {record['lag_samples']}-sample constant lag before measuring "
                f"(confidence {record['confidence']}).")
    if policy == schema.COMPARE_ALIGN_VARISPEED and record.get("applied"):
        return (f"Aligned: undid a declared {record['declared_ratio']}× varispeed by resampling the "
                f"candidate to the reference time base (observed {record['observed_ratio']}×).")
    if policy == schema.COMPARE_ALIGN_STRETCH and record.get("applied"):
        return (f"Declared {record['declared_ratio']}× pitch-preserving stretch (observed "
                f"{record['observed_ratio']}×): measured warp-invariant axes directly; graininess + "
                "corroboration warp-normalized.")
    if policy == schema.COMPARE_ALIGN_PITCH and record.get("applied"):
        return (f"Declared {record['declared_semitones']:+g}-semitone pitch shift (duration preserved): "
                "tonal-balance compensated for the expected centroid move; corroboration uses the "
                "shift-compensated spectral distance.")
    if policy == schema.COMPARE_ALIGN_NOT_ALIGNED:
        return f"Alignment refused ({record.get('reason', 'low confidence')}) — measured unaligned."
    return None
