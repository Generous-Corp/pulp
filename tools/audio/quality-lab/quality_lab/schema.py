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

SCHEMA_VERSION = 1


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

    Top-level `worst_regions` is each detector's own #1 region (already sorted within a
    detector, in that detector's unit). We deliberately do NOT cross-sort by raw
    severity — `ms` and `deficit_0to1` are not comparable numbers."""
    top = [d.worst_regions[0].to_dict() for d in detectors if d.worst_regions and d.fired]
    return {
        "schema_version": SCHEMA_VERSION,
        "case": case.to_dict(),
        "verdict": verdict,
        "detectors": [d.to_dict() for d in detectors],
        "worst_regions": top,
        "determinism": determinism,
        "provenance": provenance,
    }
