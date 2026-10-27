"""Human-readable summaries for versioned differential reports."""

from pathlib import Path
from typing import Any


def format_summary(report: dict[str, Any]) -> str:
    comparison = report["comparison"]
    timings = report["timings"]
    promotion = report["promotion"]
    lines = [
        f"# Import differential: {Path(report['source']['path']).name}",
        "",
        f"- Recommendation: **{promotion['classification']}** (advisory only)",
        f"- Threshold eligible: {promotion['threshold_eligible']}",
        f"- Browser import: {timings['browser_import_ms']} ms",
        f"- Native import: {timings['native_import_ms']} ms",
        f"- Native render: {timings['native_render_ms']} ms",
        f"- Import-only speedup: {timings['browser_to_native_import_speedup']}x",
        f"- TTFP: {report['observability']['ttfp']['status']}",
        f"- TTNI: {report['observability']['ttni']['status']}",
        f"- TTNI proxy: {report['observability']['ttni_proxy']['value_ms']} ms",
        f"- IFNF: {report['observability']['ifnf']['status']}",
        f"- Cache state: {report['observability']['cache_state']['identity']}",
        f"- Timing budget: {report['timing_budget']['status']}",
    ]
    lines += [
        f"- {name.title()} score: {comparison[name]['score']:.3f}"
        if name != "geometry" else
        f"- Page-space geometry (gated): "
        f"{comparison[name]['score']:.3f}"
        for name in ("structural", "geometry", "typography", "visual")
    ]
    if comparison["visual"].get("registered_visual"):
        lines.append(
            f"- Registered authored-frame visual score: "
            f"{comparison['visual']['registered_visual']['score']:.3f}")
    reference = comparison["visual"].get("reference")
    if isinstance(reference, dict):
        moment = reference["capture_moment_residual"]
        lines.append(
            f"- Visual reference: {reference['source']} "
            f"(DOM snapshots {'match' if reference['dom_snapshots_match'] else 'differ'}; "
            f"{moment['differing_pixel_count']} pixels differ between the two captures)")
    full_frame = comparison["visual"].get("full_frame_classification")
    if isinstance(full_frame, dict):
        detail = ""
        if "shifted_score" in full_frame:
            detail = (f" (shifted {full_frame['shifted_score']:.3f}, "
                      f"{full_frame['shifted_outside_frame']['differing_pixel_count']} "
                      "outside-frame pixels differ)")
        lines.append(
            f"- Full-frame classification: {full_frame['classification']}{detail}")
    geometry = comparison["geometry"]
    gate = geometry.get("gate") if isinstance(geometry.get("gate"), dict) else None
    lines.append(
        "- Geometry gate: "
        + ("pass" if gate and gate.get("ok") is True else
           "fail (" + (", ".join(gate.get("failed", [])) if gate else "not evaluated") + ")"))
    if gate:
        bound = gate["checks"]["registered_max_abs_delta"]
        lines.append(
            f"- Registered geometry: mean {gate['score']:.3f}, max edge delta "
            f"{bound['max_abs_delta']} px over {gate['matched_bounds']} matched boxes")
    form_controls = comparison.get("form_controls") or {}
    if promotion.get("boundary_overrides_passing_gates"):
        lines.append(
            "- Every numeric gate passes; the classification comes from the "
            "support boundary below")
    for boundary in form_controls.get("support_boundaries", []):
        lines.append(
            f"- Support boundary `{boundary['boundary']}`: "
            f"{boundary['control_count']} control(s), "
            f"{boundary['foreground_error_share'] * 100:.1f}% of scored "
            "foreground error (scores unchanged)")
    lines += [
        "",
        "Chromium remains authoritative. This report never substitutes or "
        "overwrites the canonical import.",
        "",
    ]
    if report["classifications"]:
        lines += ["## Ranked likely gaps", ""]
        for index, finding in enumerate(report["classifications"], 1):
            lines.append(
                f"{index}. `{finding['cause']}` "
                f"(confidence {finding['confidence']:.2f}): "
                f"{finding['supporting_evidence']}")
        lines.append("")
    return "\n".join(lines)


def format_corpus_summary(aggregate: dict[str, Any]) -> str:
    scores = aggregate["mean_scores"]
    timings = aggregate["mean_timings_ms"]
    counts = aggregate["classifications"]
    lines = [
        "# Importer Differential Lab corpus report", "",
        f"- Fixtures: {aggregate['fixture_count']}",
        f"- Completed: {aggregate['completed_fixture_count']}",
        f"- Failed/unsupported: {aggregate['failed_fixture_count']}",
        f"- Mean browser import: {timings['browser_import']:.1f} ms",
        f"- Mean native import: {timings['native_import']:.1f} ms",
        f"- Mean native render: {timings['native_render']:.1f} ms",
        f"- Mean import-only speedup: {aggregate['mean_import_only_speedup']:.2f}x",
        f"- TTFP: {aggregate['observability']['ttfp']['status']}",
        f"- TTNI: {aggregate['observability']['ttni']['status']}",
        f"- TTNI proxy p95: {aggregate['observability']['ttni_proxy']['p95_ms']} ms",
        f"- IFNF: {aggregate['observability']['ifnf']['status']}",
        f"- Cache states: {', '.join(aggregate['observability']['cache_states']) or 'none'}",
        f"- Timing budget: {aggregate['timing_budget']['status']}",
        f"- Native-authoritative: {counts['native-authoritative']}",
        f"- Native with browser validation: {counts['native-with-browser-validation']}",
        f"- Browser-required: {counts['browser-required']}",
        f"- False promotions: {aggregate['false_promotions']} "
        f"({aggregate.get('false_promotion_status', 'status unavailable')})", "",
        "## Mean scores", "",
    ]
    lines += [
        f"- {name.title()}: {scores[name]:.3f}"
        for name in ("structural", "geometry", "typography", "visual")
    ]
    registered = aggregate.get("registered_visual", {})
    if registered.get("fixture_count"):
        lines.append(
            f"- Registered authored-frame visual: {registered['mean_score']:.3f} "
            f"({registered['fixture_count']} fixtures)")
    raw_geometry = aggregate.get("diagnostics", {}).get("raw_page_space_geometry", {})
    if raw_geometry.get("fixture_count"):
        lines.append(
            f"- Page-space geometry (gated): mean "
            f"{raw_geometry['mean_score']:.3f}, minimum {raw_geometry['min_score']:.3f}")
    gate_summary = aggregate.get("gate_summary", {})
    if gate_summary:
        lines += ["", "## Gate coverage", ""]
        for name, row in gate_summary.items():
            lines.append(
                f"- {name}: {row['pass_count']}/{row['fixture_count']} pass, "
                f"minimum {row['min_score'] if row['min_score'] is not None else 'n/a'}, "
                f"threshold {row['threshold']:.3f}")
    lines += ["", "## Worst fixtures by visual score", ""]
    for row in aggregate.get("worst_fixtures", []):
        registered_score = (f"{row['registered_visual']:.3f}"
                            if row["registered_visual"] is not None else "n/a")
        lines.append(
            f"- `{row['fixture']}` — visual {row['scores']['visual']:.3f}, "
            f"registered {registered_score}, "
            f"geometry {row['scores']['geometry']:.3f}, "
            f"typography {row['scores']['typography']:.3f}, "
            f"classification `{row['classification']}`")
    if aggregate.get("worst_registered_visual"):
        lines += ["", "## Worst fixtures by registered visual score", ""]
        for row in aggregate["worst_registered_visual"]:
            lines.append(
                f"- `{row['fixture']}` — registered visual "
                f"{row['registered_visual']:.3f}, classification "
                f"`{row['classification']}`")
    boundary_rows = aggregate.get("support_boundary_fixtures", [])
    if boundary_rows:
        lines += ["", "## Support boundaries", ""]
        for row in boundary_rows:
            shares = ", ".join(
                f"`{b['boundary']}` {b['foreground_error_share'] * 100:.1f}%"
                for b in row["boundaries"]
                if isinstance(b, dict))
            note = (" — every numeric gate passes"
                    if row["boundary_overrides_passing_gates"] else "")
            lines.append(f"- `{row['fixture']}`: {shares}{note}")
    lines += ["", "## Ranked gaps", ""]
    for index, gap in enumerate(aggregate["ranked_gaps"], 1):
        lines.append(
            f"{index}. `{gap['cause']}` — {gap['affected']}/"
            f"{aggregate['fixture_count']} fixtures, priority "
            f"{gap['priority_score']:.3f}, subsystem "
            f"`{gap['suggested_subsystem']}`")
    lines += [
        "",
        "Chromium remains authoritative; production native promotion is disabled.",
        "",
    ]
    return "\n".join(lines)
