"""Stable seams for the Audio Quality Lab: the QualityCase and the report envelope.

Per the plan's architectural guardrails (§14.4), the *schemas* are the public API of
the lab — detectors, alignment, and report renderers are swappable behind them. Keep
every schema here, in one place, and version it. Nothing in this module imports a
detector or a stage; it is pure data shape.

See planning/2026-06-26-audio-quality-lab-perceptual-harness.md §3.5 (QualityCase),
§7 (report), §7.1 (provenance).
"""
from __future__ import annotations

from dataclasses import dataclass, field, asdict
from typing import Any

# Bumped to 2 for the maturity gate: detector results carry `maturity` +
# `participates_in_verdict` / `participates_in_engine_baseline`, and the report gains an
# `advisory` namespace for non-gating output (experimental detectors, reviewers, …).
SCHEMA_VERSION = 2

# The maturity gate's participation policy — the SINGLE source of truth. Every verdict /
# gate path routes through `detectors_for_verdict` / `detectors_for_engine_baseline` so no
# call site can forget the rule.
VERDICT_MATURITIES = frozenset({"beta", "stable"})       # experimental excluded
ENGINE_BASELINE_MATURITIES = frozenset({"stable"})       # experimental + beta excluded


def participates_in_verdict(result: "DetectorResult") -> bool:
    return result.maturity in VERDICT_MATURITIES


def participates_in_engine_baseline(result: "DetectorResult") -> bool:
    return result.maturity in ENGINE_BASELINE_MATURITIES


def detectors_for_verdict(results: "list[DetectorResult]") -> "list[DetectorResult]":
    """The detectors whose fired/low_coverage may move the headline verdict."""
    return [r for r in results if participates_in_verdict(r)]


def detectors_for_engine_baseline(results: "list[DetectorResult]") -> "list[DetectorResult]":
    """The detectors captured into / checked against the regression baseline."""
    return [r for r in results if participates_in_engine_baseline(r)]


@dataclass
class QualityCase:
    """One unit of work: how a candidate is rendered, what 'correct' is, how it is
    aligned, and which detectors apply. Time-stretch is one family (§3.5); the same
    shape serves pitch-shift / freeze / synth / effect by changing the policy fields.
    """

    case_id: str
    family: str  # "time-stretch" | "pitch-shift" | "freeze" | "synth" | "effect"
    reference_policy: str  # "frozen-reference" | "dry-input" | "analytic" | "reference-free"
    alignment_policy: str  # "identity" | "fixed-latency-trim" | "onset-map" | "ratio-map" | "constrained-dtw"
    detector_tags: list[str] = field(default_factory=list)
    params: dict[str, Any] = field(default_factory=dict)  # e.g. {"ratio": 1.5}

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass
class WorstRegion:
    """A localized defect: a timestamp (seconds, in the report's stated time domain)
    plus the detector's severity there and a short human label."""

    time_s: float
    severity: float
    detector: str
    label: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "time_s": round(self.time_s, 4),
            "severity": round(self.severity, 4),
            "detector": self.detector,
            "label": self.label,
        }


@dataclass
class DetectorResult:
    """A single detector's verdict for one case.

    `scalar` is the regression number a gate asserts against; `curve` is the per-frame
    (time_s, value) series for localization/heatmaps; `time_domain` records which
    domain the curve lives in (§4.5.1) so a reader never misinterprets it.
    """

    name: str
    scalar: float
    unit: str
    fired: bool
    time_domain: str  # "aligned" | "source-time" | "raw-output"
    measured: int = 0  # onset pairs the detector actually scored
    expected: int = 0  # onset pairs offered by the alignment layer
    curve: list[tuple[float, float]] = field(default_factory=list)
    worst_regions: list[WorstRegion] = field(default_factory=list)
    tolerance_class: str = ""
    notes: str = ""
    # Confidence tier (the maturity gate). `experimental` detectors run and report but
    # are advisory: their `fired`/`low_coverage` never move the verdict or the regression
    # gate. `beta` participates in the verdict but is held out of `engine_baseline` by
    # default. `stable` participates everywhere. New detectors ship `experimental` and are
    # promoted only after their validation clears a bar. Existing detectors default stable.
    maturity: str = "stable"

    # Below this fraction, a "clean" verdict is untrustworthy — the detector simply
    # didn't see enough (boundary skips, failed matches). Surfaced, never hidden.
    MIN_COVERAGE = 0.5

    @property
    def coverage(self) -> float:
        return self.measured / self.expected if self.expected else 0.0

    @property
    def low_coverage(self) -> bool:
        return self.expected > 0 and self.coverage < self.MIN_COVERAGE

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "scalar": round(self.scalar, 6),
            "unit": self.unit,
            "fired": bool(self.fired),
            "time_domain": self.time_domain,
            "coverage": round(self.coverage, 3),
            "measured": self.measured,
            "expected": self.expected,
            "low_coverage": self.low_coverage,
            "tolerance_class": self.tolerance_class,
            "curve": [[round(t, 4), round(v, 6)] for t, v in self.curve],
            "worst_regions": [w.to_dict() for w in self.worst_regions],
            "notes": self.notes,
            # Gate participation is explicit in the JSON — readers must never infer it
            # from array position or from `fired`.
            "maturity": self.maturity,
            "participates_in_verdict": participates_in_verdict(self),
            "participates_in_engine_baseline": participates_in_engine_baseline(self),
        }


def build_report(
    case: QualityCase,
    detectors: list[DetectorResult],
    provenance: dict[str, Any],
    determinism: dict[str, Any],
    verdict: str,
) -> dict[str, Any]:
    """Assemble the canonical report envelope (§7). JSON-serializable; the same shape
    every later layer (perceptual models, listening infra, LLM reviewer) extends.

    Top-level `worst_regions` is each VERDICT-PARTICIPATING detector's own #1 region
    (already sorted within a detector, in that detector's unit). We deliberately do NOT
    cross-sort by raw severity — `ms` and `deficit_0to1` are not comparable numbers — and
    experimental (advisory) detectors are excluded so they can't surface a headline region.

    `detectors` keeps ALL detectors (each carries explicit participation flags); the
    `advisory` namespace holds non-gating output (experimental detectors now; reviewers and
    perceptual models later) so a reader sees everything but the gate sees only what
    participates."""
    gated = detectors_for_verdict(detectors)
    advisory_dets = [d for d in detectors if not participates_in_verdict(d)]
    top = [d.worst_regions[0].to_dict() for d in gated if d.worst_regions and d.fired]
    return {
        "schema_version": SCHEMA_VERSION,
        "case": case.to_dict(),
        "verdict": verdict,
        "detectors": [d.to_dict() for d in detectors],
        "worst_regions": top,
        # Reserved non-gating namespace. `detectors` = experimental/advisory detector
        # results; `reviewers` (#5296) and `perceptual` (Layer B) are reserved shapes.
        "advisory": {
            "detectors": [d.to_dict() for d in advisory_dets],
            "reviewers": [],
            "perceptual": [],
        },
        "determinism": determinism,
        "provenance": provenance,
    }


# ── Compare report schema (agent-facing before/after judgment) ──────────────────────────
# The `compare` surface (quality_lab/compare.py) owns the MEASUREMENT logic; this module
# owns its SHAPE, like every other lab schema. The constructor below is the single place a
# measurement envelope is built, so `measured` / `not_applicable` / `invalid` always share
# one contract — a reader (and the future MCP tool) never special-cases the shape. It is
# axis-agnostic: the caller supplies `alignment`/`region`, so no tonal-balance assumption
# leaks into the schema.
COMPARE_SCHEMA = "quality_lab.compare.v1"
# Evolution policy for this envelope: keys may be ADDED within `v1` (readers must ignore unknown
# keys; the MCP round-trips the JSON with no key whitelist, the C++ CLI byte-passes it, and no
# consumer asserts an exact key set). A REMOVAL or RENAME of an existing key is a breaking change
# and bumps the schema version. `headline_flags` (below) is such an additive extension.

# Action-oriented verdicts an agent can act on — advisory, never a gate.
COMPARE_VERDICT_REGRESSION = "regression_suspected"
COMPARE_VERDICT_MATERIAL = "material_change_detected"
COMPARE_VERDICT_NO_CHANGE = "no_material_change_detected"
COMPARE_VERDICT_INCONCLUSIVE = "inconclusive"
COMPARE_VERDICT_INVALID = "invalid"

# Per-measurement status (rolls up into the verdict).
COMPARE_STATUS_MEASURED = "measured"
COMPARE_STATUS_NOT_APPLICABLE = "not_applicable"
COMPARE_STATUS_INVALID = "invalid"

# Corroboration is a MATERIALITY cross-check, never a trust score (see compare.py and the
# plan's "agreement != trust" non-goal). It reports only whether an independent, algorithm-
# agnostic raw measure ALSO registers a material change — agreement/disagreement about
# *materiality*, under the same level-matched global contract. It NEVER moves the verdict.
COMPARE_CORROBORATED = "corroborated"
COMPARE_NOT_CORROBORATED = "not_corroborated"
COMPARE_CORROBORATION_NA = "not_applicable"

# Alignment is a measurement PRECONDITION (optionally trim/warp to a common time base before
# measuring), disclosed on the envelope's `alignment` field — NEVER a verdict input. The policy
# names live here; later tiers add onset-map / warp policies behind the same record shape.
COMPARE_ALIGN_NOT_REQUIRED = "not_required"      # alignment-free axis, or not requested
COMPARE_ALIGN_NOT_ALIGNED = "not_aligned"        # requested but refused (low confidence / declaration unsupported)
COMPARE_ALIGN_FIXED_LATENCY = "fixed-latency-trim"  # a single constant lag was trimmed
COMPARE_ALIGN_VARISPEED = "varispeed-resample"   # a declared varispeed ratio was undone by resampling


def compare_alignment(policy: str, **fields: Any) -> dict[str, Any]:
    """One alignment record for a measurement envelope. Returns a FRESH dict every call (never a
    shared singleton — a consumer mutating one report's alignment must not corrupt others or a
    module constant). `fields` carry policy-specific keys (e.g. `lag_samples`/`confidence` on a
    trim, `reason` on a refusal). Shape is owned here so later alignment policies stay consistent."""
    return {"policy": policy, **fields}


def compare_alignment_not_required(reason: str = "global_ltas_metric", **fields: Any) -> dict[str, Any]:
    """The default alignment record for an alignment-free measurement (or `--align none`)."""
    return compare_alignment(COMPARE_ALIGN_NOT_REQUIRED, reason=reason, **fields)


def compare_measurement(
    axis: str,
    tool: str,
    status: str,
    *,
    applicable: bool,
    alignment: dict[str, Any],
    region: str = "global",
    reason: str | None = None,
    **evidence: Any,
) -> dict[str, Any]:
    """Build one compare evidence envelope with the common contract keys ALWAYS present.
    `measured` envelopes pass extra evidence (level_match/materiality/payload) via
    **evidence; `not_applicable`/`invalid` pass a `reason`. Never hand-build one elsewhere."""
    env: dict[str, Any] = {
        "axis": axis,
        "tool": tool,
        "role": "detector",
        "status": status,
        "applicable": applicable,
        "alignment": alignment,
        "region": region,
        "coverage": 1.0 if status == COMPARE_STATUS_MEASURED else 0.0,
        "can_support_verdict": status == COMPARE_STATUS_MEASURED and applicable,
    }
    if reason is not None:
        env["reason"] = reason
    env.update(evidence)
    return env


def compare_downmix_note(ref_channels: int, cand_channels: int) -> dict[str, Any]:
    """Disclosure that multichannel input(s) were mean-downmixed to mono before measuring.

    Attached to the measurement envelope whenever either input had more than one channel, so a
    reader knows stereo/spatial changes were NOT compared (honesty-per-measurement: the report
    states what it could not see, rather than silently returning 'no change'). Shape only."""
    return {
        "applied": True,
        "ref_channels": int(ref_channels),
        "cand_channels": int(cand_channels),
        "note": "stereo/spatial image not compared — multichannel input(s) were downmixed to mono",
    }


def compare_raw_comparator(
    name: str,
    tool: str,
    unit: str,
    note: str,
    *,
    value: float | None = None,
    ref_value: float | None = None,
    cand_value: float | None = None,
    delta: float | None = None,
    detail: dict[str, Any] | None = None,
    maturity: str = "experimental",
) -> dict[str, Any]:
    """One deterministic, off-gate raw measurement for the report's `advisory` namespace.

    Raw comparators are algorithm-agnostic scalars (a relational `value` like a null-residual,
    or a per-side `ref_value`/`cand_value` + `delta` like a tempo estimate). `detail` carries
    measurement-specific metadata the reader needs to judge the scalar (e.g. how many samples a
    null-residual actually compared, so a truncated render can't masquerade as an identity
    match). They make NO good/bad judgment and NEVER participate in the verdict — `maturity`
    defaults to `experimental` to make that explicit, mirroring the DetectorResult maturity gate."""
    rc: dict[str, Any] = {"name": name, "tool": tool, "unit": unit, "maturity": maturity,
                          "participates_in_verdict": False, "note": note}
    if value is not None:
        rc["value"] = value
    if ref_value is not None:
        rc["ref_value"] = ref_value
    if cand_value is not None:
        rc["cand_value"] = cand_value
    if delta is not None:
        rc["delta"] = delta
    if detail is not None:
        rc["detail"] = detail
    return rc


# Headline flags — machine-readable top-level pointers that PROMOTE an advisory corroboration
# DISAGREEMENT into the headline without moving the verdict. Structured (not a bare prose line) so
# the known false-alarm class is machine-SUPPRESSIBLE: a caller doing time/pitch-variant processing
# (where the phase-sensitive residual always disagrees with a tonal axis) knows
# `uncaptured_material_difference` is expected and filters it by `expected_for`. NEVER read by the
# verdict — the advisory/verdict boundary is a hard contract.
COMPARE_FLAG_UNCAPTURED_DIFF = "uncaptured_material_difference"
COMPARE_FLAG_AXIS_ONLY = "axis_change_without_residual"


def compare_headline_flag(
    flag: str, detail: str, *, expected_for: list[str] | None = None
) -> dict[str, Any]:
    """One structured headline flag. `flag` is a stable machine token (COMPARE_FLAG_*); `detail`
    is a human line; `expected_for` names processing classes for which this flag is a known,
    suppressible false alarm (e.g. `time_variant_processing`). Shape only."""
    return {"flag": flag, "detail": detail, "expected_for": list(expected_for or [])}


def compare_corroboration(status: str, note: str, *, basis: dict[str, Any] | None = None) -> dict[str, Any]:
    """A materiality cross-check result. `status` is one of COMPARE_CORROBORATED /
    COMPARE_NOT_CORROBORATED / COMPARE_CORROBORATION_NA. `note` MUST state that this is a
    materiality agreement under the level-matched global contract, not a trust score; `basis`
    records the raw comparators consulted so the check is auditable."""
    c: dict[str, Any] = {"status": status, "participates_in_verdict": False, "note": note}
    if basis is not None:
        c["basis"] = basis
    return c


def compare_advisory(
    raw_comparators: list[dict[str, Any]] | None = None,
    corroboration: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """The report's non-gating namespace: deterministic raw comparators + an optional
    materiality corroboration. Reserved and never consulted by the verdict — the compare peer
    of `build_report`'s `advisory` block."""
    return {"raw_comparators": raw_comparators or [], "corroboration": corroboration}


def compare_report(
    profile: str,
    reference_role: str,
    verdict: str,
    summary: str,
    measurements: list[dict[str, Any]],
    provenance: dict[str, Any] | None = None,
    advisory: dict[str, Any] | None = None,
    headline_flags: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Assemble the agent-facing compare report envelope. `provenance` is attached whenever
    available — including on `invalid` reports — so an agent can always trace what it read.
    `advisory` (raw comparators + corroboration) is attached when present; it is off-gate and
    never consulted by the verdict. `headline_flags` is ALWAYS present (empty when there is no
    corroboration disagreement to promote) so a machine reader has one stable place to look; it too
    never moves the verdict."""
    report: dict[str, Any] = {
        "schema": COMPARE_SCHEMA,
        "profile": profile,
        "reference_role": reference_role,
        "verdict": verdict,
        "summary": summary,
        "measurements": measurements,
        "headline_flags": headline_flags or [],
    }
    if advisory is not None:
        report["advisory"] = advisory
    if provenance is not None:
        report["provenance"] = provenance
    return report
