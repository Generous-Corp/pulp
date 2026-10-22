"""Root-cause hypotheses and the advisory promotion verdict for one fixture."""

from __future__ import annotations

from typing import Any

from .geometry_gate import geometry_gate_failures, geometry_gate_ok
from .model import PROMOTION_THRESHOLDS
from .visual import full_frame_ok

ROOT_CAUSE_ORDER = [
    "javascript-evaluation", "conditional-rendering", "async-initialization",
    "canvas", "webgl", "pseudo-element", "css-variable", "responsive-style",
    "flex-layout", "grid-layout", "intrinsic-sizing", "absolute-positioning",
    "font-resolution", "font-metrics", "text-wrapping", "svg",
    "image-loading", "transform", "animation", "portal",
    "ua-default-form-chrome", "ua-shadow-form-part",
    "form-control-appearance-unrecorded",
    "unsupported-browser-api", "source-recognition", "unknown",
]


def classify_gaps(
    recognition: dict[str, Any],
    structural: dict[str, Any],
    geometry: dict[str, Any],
    typography: dict[str, Any],
    visual: dict[str, Any],
    form_control_report: dict[str, Any] | None = None,
) -> list[dict[str, Any]]:
    features = recognition["features"]
    facts: dict[str, tuple[str, float, str]] = {}
    if form_control_report:
        for boundary in form_control_report.get("support_boundaries", []):
            what = ("painted by Chromium's native theme (appearance is not none)"
                    if boundary["boundary"] == "ua-default-form-chrome" else
                    "drawing parts in a user-agent shadow tree the snapshot "
                    "does not serialize")
            facts[boundary["boundary"]] = (
                f"{boundary['control_count']} form control(s) "
                f"({', '.join(boundary['controls'])}) {what}; "
                f"{boundary['foreground_error_share'] * 100:.1f}% of the scored "
                "foreground error lies inside their boxes",
                0.9,
                "form-control-appearance",
            )
        unrecorded = [
            row for row in form_control_report.get("controls", [])
            if row["status"] in ("appearance-unrecorded", "theme-unresolved")]
        if unrecorded:
            facts["form-control-appearance-unrecorded"] = (
                f"{len(unrecorded)} form control(s) have no captured "
                "appearance or theme verdict, so whether the native theme "
                "paints them is unknown; capture again",
                0.5,
                "browser-capture",
            )
    mappings = {
        "canvas": ("canvas", 1.0),
        "webgl": ("webgl", 1.0),
        "pseudo_elements": ("pseudo-element", 0.95),
        "css_variables": ("css-variable", 0.9),
        "media_queries": ("responsive-style", 0.9),
        "flex_layout": ("flex-layout", 0.85),
        "grid_layout": ("grid-layout", 0.85),
        "absolute_positioning": ("absolute-positioning", 0.85),
        "custom_fonts": ("font-resolution", 0.8),
        "svg": ("svg", 0.8),
        "images": ("image-loading", 0.8),
        "animation": ("animation", 0.95),
        "portals": ("portal", 0.95),
        "async_initialization": ("async-initialization", 0.95),
        "conditional_rendering": ("conditional-rendering", 0.9),
    }
    for feature, (cause, confidence) in mappings.items():
        if features.get(feature):
            facts[cause] = (
                f"source recognition observed {feature.replace('_', ' ')}",
                confidence,
                "source-recognition",
            )
    if recognition["framework"] in {"react", "vue", "svelte"}:
        facts["javascript-evaluation"] = (
            f"{recognition['framework']} source requires evaluated component state",
            0.98,
            "runtime-importer",
        )
    if structural["score"] < PROMOTION_THRESHOLDS["structural"]:
        facts.setdefault("source-recognition", (
            f"{structural['missing_count']} browser nodes were not matched",
            0.75,
            "parser",
        ))
    if not geometry_gate_ok(geometry):
        likely = "flex-layout" if features.get("flex_layout") else (
            "grid-layout" if features.get("grid_layout") else "intrinsic-sizing")
        facts.setdefault(likely, (
            f"geometry gate failed ({geometry['score']:.3f} raw page-space): "
            f"{', '.join(geometry_gate_failures(geometry))}",
            0.65,
            "layout",
        ))
    if typography["score"] < PROMOTION_THRESHOLDS["typography"]:
        facts.setdefault("font-metrics", (
            f"typography score was {typography['score']:.3f}",
            0.6,
            "text",
        ))
    if visual["score"] < PROMOTION_THRESHOLDS["visual"] and not facts:
        facts["unknown"] = (
            f"visual score was {visual['score']:.3f}",
            0.3,
            "unknown",
        )
    result = []
    for cause in ROOT_CAUSE_ORDER:
        if cause not in facts:
            continue
        evidence, confidence, subsystem = facts[cause]
        result.append({
            "cause": cause,
            "status": "hypothesis",
            "confidence": confidence,
            "supporting_evidence": evidence,
            "suggested_subsystem": subsystem,
        })
    return result


def promotion_recommendation(
    unsupported: list[str],
    structural: dict[str, Any],
    geometry: dict[str, Any],
    typography: dict[str, Any],
    visual: dict[str, Any],
    boundaries: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Advisory classification. Support boundaries never change a score.

    A support boundary is content Pulp's renderer cannot reproduce from the
    capture (for example a control the native theme paints). It makes the
    fixture browser-required and ineligible, while every score — including the
    residual the boundary explains — stays reported and counted.
    """
    boundaries = boundaries or []
    registered_visual = visual.get("registered_visual")
    visual_gate_score = (
        registered_visual["score"] if registered_visual else visual["score"])
    scores = {
        "structural": structural["score"],
        "geometry": geometry["score"],
        "typography": typography["score"],
        "visual": visual_gate_score,
    }
    failed = [
        name for name, threshold in PROMOTION_THRESHOLDS.items()
        if name != "geometry" and scores[name] < threshold
    ]
    geometry_failures = geometry_gate_failures(geometry)
    if not geometry_gate_ok(geometry):
        failed.insert(1 if "structural" in failed else 0, "geometry")
    full_frame_classification = (
        visual.get("full_frame_classification") or {}).get(
            "classification", "unclassified")
    if not full_frame_ok(visual):
        failed.append("full_frame")

    def failure_reason(name: str) -> str:
        if name == "geometry":
            return f"geometry gate failed: {', '.join(geometry_failures)}"
        if name == "full_frame":
            return (f"full-frame visual {visual['score']:.3f} below "
                    f"{PROMOTION_THRESHOLDS['visual']:.3f} and classified "
                    f"{full_frame_classification}")
        return f"{name} score below {PROMOTION_THRESHOLDS[name]:.3f}"

    threshold_eligible = not unsupported and not failed and not boundaries
    # A boundary that reclassifies a fixture whose every numeric gate passed is
    # a decision, not a measurement, so it is named as one in the report.
    boundary_overrides_passing_gates = bool(
        boundaries and not unsupported and not failed)
    if unsupported or boundaries:
        classification = "browser-required"
        reasons = []
        if unsupported:
            reasons.append(
                f"unsupported dynamic features: {', '.join(unsupported)}")
        if boundary_overrides_passing_gates:
            reasons.append(
                "every numeric gate passes; classified browser-required by "
                "the support boundary below, not by a score")
        for boundary in boundaries:
            reasons.append(
                f"support boundary {boundary['boundary']}: "
                f"{boundary['control_count']} control(s), "
                f"{boundary['foreground_error_share'] * 100:.1f}% of scored "
                "foreground error")
        reasons += [failure_reason(name) for name in failed]
    else:
        classification = "native-with-browser-validation"
        reasons = (
            [failure_reason(name) for name in failed]
            if failed else
            ["threshold-eligible, but production promotion is disabled until "
             "a candidate-only classifier passes a frozen holdout corpus"])
    return {
        "classification": classification,
        "advisory_only": True,
        "production_promotion_enabled": False,
        "threshold_eligible": threshold_eligible,
        "thresholds": PROMOTION_THRESHOLDS,
        "visual_gate_score": visual_gate_score,
        "visual_full_frame_score": visual["score"],
        "visual_registered_score": (
            registered_visual["score"] if registered_visual else None),
        "geometry_gate_ok": geometry_gate_ok(geometry),
        "full_frame_ok": full_frame_ok(visual),
        "full_frame_classification": full_frame_classification,
        "geometry_gate_failed": geometry_failures,
        "support_boundaries": [
            {"boundary": b["boundary"], "control_count": b["control_count"],
             "foreground_error_share": b["foreground_error_share"]}
            for b in boundaries],
        "numeric_gates_pass": not failed,
        "boundary_overrides_passing_gates": boundary_overrides_passing_gates,
        "reasons": reasons,
    }

