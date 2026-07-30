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
    ]
    lines += [
        f"- {name.title()} score: {comparison[name]['score']:.3f}"
        for name in ("structural", "geometry", "typography", "visual")
    ]
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
        f"- Native-authoritative: {counts['native-authoritative']}",
        f"- Native with browser validation: {counts['native-with-browser-validation']}",
        f"- Browser-required: {counts['browser-required']}",
        f"- False promotions: {aggregate['false_promotions']}", "",
        "## Mean scores", "",
    ]
    lines += [
        f"- {name.title()}: {scores[name]:.3f}"
        for name in ("structural", "geometry", "typography", "visual")
    ]
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
