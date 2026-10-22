"""Corpus aggregation: shares, gate summaries, tails, and ranked gaps."""

from __future__ import annotations

import math
import statistics
from typing import Any

from .geometry_gate import geometry_gate_ok
from .model import PROMOTION_THRESHOLDS, SCHEMA_VERSION

CORPUS_SCHEMA = "pulp-importer-differential-corpus-v1"


def percentile(values: list[int], fraction: float) -> int:
    ordered = sorted(values)
    return ordered[max(0, min(len(ordered) - 1,
                               math.ceil(len(ordered) * fraction) - 1))]


def aggregate_reports(
    reports: list[dict[str, Any]], failure_count: int = 0
) -> dict[str, Any]:
    classifications = {
        "native-authoritative": 0,
        "native-with-browser-validation": 0,
        "browser-required": 0,
        "unsupported": 0,
    }
    gap_counts: dict[str, dict[str, Any]] = {}
    for report in reports:
        classifications[report["promotion"]["classification"]] += 1
        impact = 1.0 - report["comparison"]["visual"]["score"]
        for finding in report["classifications"]:
            bucket = gap_counts.setdefault(finding["cause"], {
                "cause": finding["cause"],
                "affected": 0,
                "visual_impact_sum": 0.0,
                "confidence_sum": 0.0,
                "suggested_subsystem": finding["suggested_subsystem"],
            })
            bucket["affected"] += 1
            bucket["visual_impact_sum"] += impact
            bucket["confidence_sum"] += finding["confidence"]
    ranked = []
    for bucket in gap_counts.values():
        affected = bucket["affected"]
        ranked.append({
            "cause": bucket["cause"],
            "affected": affected,
            "affected_fraction": round(affected / max(1, len(reports)), 6),
            "mean_visual_impact": round(
                bucket["visual_impact_sum"] / affected, 6),
            "mean_confidence": round(bucket["confidence_sum"] / affected, 6),
            "suggested_subsystem": bucket["suggested_subsystem"],
            "priority_score": round(
                affected * (bucket["visual_impact_sum"] / affected)
                * (bucket["confidence_sum"] / affected), 6),
        })
    ranked.sort(key=lambda row: (-row["priority_score"], row["cause"]))

    # Keep the tail visible alongside means. An aggregate can look healthy
    # while one fixture is materially broken, so corpus consumers need a
    # stable worst-fixture view for every run.
    fixture_rows = [
        {
            "fixture": report.get("source", {}).get("path", "unknown"),
            "classification": report["promotion"]["classification"],
            # Older receipts may predate the explicit eligibility field.  Keep
            # aggregation backward-compatible while remaining fail-closed:
            # absence means the fixture is not eligible for promotion.
            "threshold_eligible": report["promotion"].get("threshold_eligible", False),
            "scores": {
                name: report["comparison"][name]["score"]
                for name in ("structural", "geometry", "typography", "visual")
            },
            "registered_visual": (
                report["comparison"]["visual"].get("registered_visual", {})
                or {}).get("score"),
            "geometry_gate_ok": geometry_gate_ok(report["comparison"]["geometry"]),
            "support_boundaries": report["promotion"].get("support_boundaries", []),
            "boundary_overrides_passing_gates": report["promotion"].get(
                "boundary_overrides_passing_gates", False),
            "registered_geometry": (
                report["comparison"]["geometry"].get("gate") or {}).get("score"),
        }
        for report in reports
    ]
    worst_fixtures = sorted(
        fixture_rows,
        key=lambda row: (row["scores"]["visual"], row["fixture"]),
    )[:5]
    worst_registered_visual = sorted(
        (row for row in fixture_rows if row["registered_visual"] is not None),
        key=lambda row: (row["registered_visual"], row["fixture"]),
    )

    def mean(path: tuple[str, ...]) -> float:
        values = []
        for report in reports:
            value: Any = report
            for key in path:
                value = value[key]
            values.append(float(value))
        return round(statistics.fmean(values), 6) if values else 0.0

    registered_visual_values = [
        float(report["comparison"]["visual"]["registered_visual"]["score"])
        for report in reports
        if report["comparison"]["visual"].get("registered_visual")
    ]
    gate_values: dict[str, list[float]] = {
        "structural": [float(r["comparison"]["structural"]["score"]) for r in reports],
        # The geometry gate's score is registered panel geometry; the gate
        # also requires page-space geometry, summarised beside it. A report
        # without a gate scores 0.
        "geometry": [
            float((r["comparison"]["geometry"].get("gate") or {}).get("score", 0.0))
            for r in reports],
        "typography": [float(r["comparison"]["typography"]["score"]) for r in reports],
        "registered_visual": registered_visual_values,
    }
    raw_geometry_values = [
        float(r["comparison"]["geometry"]["score"]) for r in reports]
    gate_thresholds = {
        "structural": PROMOTION_THRESHOLDS["structural"],
        "geometry": PROMOTION_THRESHOLDS["geometry"],
        "typography": PROMOTION_THRESHOLDS["typography"],
        "registered_visual": PROMOTION_THRESHOLDS["visual"],
    }

    total = len(reports) + failure_count
    classifications["unsupported"] = failure_count
    false_promotions = sum(
        1 for report in reports
        if report["promotion"]["classification"] == "native-authoritative"
        and not report["promotion"]["threshold_eligible"])
    budget_reports = [report.get("timing_budget", {}) for report in reports]
    budget_requested = any(
        report.get("status") in ("pass", "fail") for report in budget_reports)
    budget_failed = any(report.get("status") == "fail" for report in budget_reports)
    return {
        "schema": CORPUS_SCHEMA,
        "version": SCHEMA_VERSION,
        "fixture_count": total,
        "completed_fixture_count": len(reports),
        "failed_fixture_count": failure_count,
        "classifications": classifications,
        "shares": {
            key: round(value / max(1, total), 6)
            for key, value in classifications.items()
        },
        "mean_scores": {
            name: mean(("comparison", name, "score"))
            for name in ("structural", "geometry", "typography", "visual")
        },
        "registered_visual": {
            "fixture_count": len(registered_visual_values),
            "mean_score": round(statistics.fmean(registered_visual_values), 6)
            if registered_visual_values else None,
        },
        "gate_summary": {
            name: {
                "fixture_count": len(values),
                # Geometry passes on its full gate (frame placement, registered
                # score, absolute tolerance), not on the score alone.
                "pass_count": (
                    sum(geometry_gate_ok(r["comparison"]["geometry"]) for r in reports)
                    if name == "geometry" else
                    sum(value >= gate_thresholds[name] for value in values)),
                "min_score": round(min(values), 6) if values else None,
                "threshold": gate_thresholds[name],
            }
            for name, values in gate_values.items()
        },
        "diagnostics": {
            "raw_page_space_geometry": {
                "role": "gated-within-geometry-gate",
                "fixture_count": len(raw_geometry_values),
                "min_score": round(min(raw_geometry_values), 6)
                if raw_geometry_values else None,
                "mean_score": round(statistics.fmean(raw_geometry_values), 6)
                if raw_geometry_values else None,
            },
        },
        "mean_timings_ms": {
            "browser_import": mean(("timings", "browser_import_ms")),
            "native_import": mean(("timings", "native_import_ms")),
            "native_render": mean(("timings", "native_render_ms")),
            "native_total": mean(("timings", "native_total_ms")),
        },
        "mean_import_only_speedup": mean(
            ("timings", "browser_to_native_import_speedup")),
        "worst_fixtures": worst_fixtures,
        "worst_registered_visual": worst_registered_visual[:5],
        # Every fixture a support boundary reclassified, with the share of its
        # residual the boundary explains — listed in full, including the ones
        # whose numeric gates all pass, so no reclassification is silent.
        "support_boundary_fixtures": [
            {"fixture": row["fixture"],
             "boundaries": row["support_boundaries"],
             "boundary_overrides_passing_gates":
                 row["boundary_overrides_passing_gates"]}
            for row in fixture_rows if row["support_boundaries"]],
        "false_promotions": false_promotions,
        "false_promotion_status": (
            "not-evaluable-production-promotion-disabled"
            if not any(
                report["promotion"].get("production_promotion_enabled", False)
                for report in reports)
            else "evaluated"),
        "timing_budget": {
            "status": "fail" if budget_failed else
                ("pass" if budget_requested else "not-requested"),
            "budgets_ms": next((report.get("budgets_ms", {}) for report in budget_reports
                                if report.get("budgets_ms")), {}),
            "failed_fixture_count": sum(
                report.get("status") == "fail" for report in budget_reports),
        },
        "observability": {
            "ttfp": {"value_ms": None, "status": "unverified"},
            "ttni": {"p50_ms": None, "p95_ms": None, "status": "unverified"},
            "ttni_proxy": {
                "p50_ms": round(statistics.median([
                    report.get("observability", {}).get("ttni_proxy", {}).get(
                        "value_ms", report["timings"]["native_import_ms"])
                    for report in reports]), 1) if reports else None,
                "p95_ms": percentile([
                    report.get("observability", {}).get("ttni_proxy", {}).get(
                        "value_ms", report["timings"]["native_import_ms"])
                    for report in reports], 0.95) if reports else None,
                "status": "measured" if reports else "unverified",
            },
            "ifnf": {"value_ms": None, "status": "readback-only"},
            "cache_states": sorted({
                report.get("observability", {}).get("cache_state", {}).get(
                    "identity", "unknown") for report in reports}),
        },
        "ranked_gaps": ranked,
    }

