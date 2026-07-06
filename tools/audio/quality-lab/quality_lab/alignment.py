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
    if mode == "varispeed":
        if not param_str:
            raise ValueError("align 'varispeed' needs a ratio, e.g. varispeed:1.5")
        try:
            ratio = float(param_str)
        except ValueError:
            raise ValueError(f"varispeed ratio must be a number, got {param_str!r}")
        # Reject non-finite ratios: inf slips past `ratio > 0` AND makes the downstream verification
        # gate `abs(observed - ratio)/ratio` evaluate to nan (nan > tol is False), which would silently
        # disable the refusal for every candidate. nan is already caught by `> 0` — guard both.
        if not math.isfinite(ratio) or ratio <= 0.0:
            raise ValueError(f"varispeed ratio must be a positive finite number, got {ratio}")
        return AlignSpec(raw=align, mode=mode, param=ratio)
    # unreachable while _HANDLERS is none/latency/varispeed — a guard for a mis-registered handler.
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


# Mode → handler. A new declared-warp class (stretch/pitch/ratio) is ONE entry here plus one handler
# function (and, if it needs a new policy string, one constant in schema.py).
_HANDLERS: dict[str, AlignHandler] = {
    "none": _align_none,
    "latency": _align_latency,
    "varispeed": _align_varispeed,
}

# Modes that change the TIME BASE (a warp), not just a constant offset. The capability axes that skip
# a constant-lag alignment (stereo image, per-onset self-alignment) are NOT invariant to a warp — a
# varispeed stretches the attacks themselves — so a warp mode is still applied for them. New warp
# classes (stretch/pitch/ratio) join this set.
_WARP_MODES = frozenset({"varispeed"})


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
    if policy == schema.COMPARE_ALIGN_NOT_ALIGNED:
        return f"Alignment refused ({record.get('reason', 'low confidence')}) — measured unaligned."
    return None
