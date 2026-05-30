#!/usr/bin/env python3
"""Route-manifest adapters and route/primitive evidence for PulpFrontendIR reports.

This module owns the low-level primitives shared across the evidence families
(route_name, semantic_role, support_for_route, row_node_id) plus the
route-manifest row adapters and route/primitive count helpers. It must not
import the styles/state/nodes/sources evidence modules: those import from here,
so keeping the dependency direction one-way avoids an import cycle.
"""

from __future__ import annotations

import pathlib
from typing import Any

from frontend_ir_validation import (
    FALLBACK_ROUTES,
    NATIVE_ROUTES,
    PLANNED_SUPPORT_ROUTES,
    ROUTE_HYBRID,
    ROUTE_LIVE_JS,
    ROUTE_NATIVE_CPP,
    ROUTE_NATIVE_HTML,
    ROUTE_RECORDED_PAINT,
    ROUTE_UNSUPPORTED,
    SOURCE_TRUTHS,
    is_finite_number,
    is_non_negative_int,
    is_positive_int,
)


def route_name(value: str | None) -> str:
    normalized = (value or "").strip().lower().replace("-", "_")
    if normalized in {ROUTE_NATIVE_CPP, "native", "cpp", "native_host_service"}:
        return ROUTE_NATIVE_CPP
    if normalized in {ROUTE_LIVE_JS, "live", "runtime_js"}:
        return ROUTE_LIVE_JS
    if normalized in {"native_html", "native_layout", "native_tree", "native_element"}:
        return ROUTE_NATIVE_HTML
    if normalized == ROUTE_HYBRID:
        return ROUTE_HYBRID
    if normalized in {"recorded", "recorded_paint", "native_custom_paint"}:
        return ROUTE_RECORDED_PAINT
    return ROUTE_UNSUPPORTED


def source_kind(path: str) -> str:
    suffix = pathlib.Path(path).suffix.lower()
    if suffix in {".jsx", ".tsx", ".js", ".ts"}:
        return "jsx"
    if suffix in {".html", ".htm"}:
        return "html"
    if suffix == ".json":
        return "design_json"
    return "pulp_js"


def normalize_source_of_truth(value: Any) -> str:
    if not isinstance(value, str) or not value:
        return "local_file"
    if value in SOURCE_TRUTHS:
        return value
    if value in {"archived_corpus_fixture", "archived_fixture_file"}:
        return "archived_fixture"
    return "local_file"


def semantic_role(row: dict[str, Any]) -> str:
    primitive = row.get("required_native_primitive")
    if isinstance(primitive, str) and primitive:
        return primitive
    family = row.get("source_component_family") or row.get("source_component_name")
    if isinstance(family, str) and family:
        return family.strip().lower().replace(" ", "_")
    return "unknown"


def support_for_route(route: str) -> dict[str, str]:
    support = {name: "planned" for name in sorted(PLANNED_SUPPORT_ROUTES)}
    if route in support:
        support[route] = "present"
    return support


def route_rows(route_manifest: dict[str, Any]) -> list[Any]:
    overlay = route_manifest.get("source_contract_overlay", {})
    if not isinstance(overlay, dict):
        return []
    rows = overlay.get("node_route_rows")
    if isinstance(rows, list) and rows:
        return rows
    rows = overlay.get("route_rows")
    if isinstance(rows, list):
        return rows
    return []


def routes_present(rows: list[Any]) -> list[str]:
    found = sorted({
        route_name(row.get("route_type"))
        for row in rows
        if isinstance(row, dict)
    })
    return found or [ROUTE_HYBRID]


def row_node_id(row: dict[str, Any], index: int) -> str:
    value = row.get("id")
    if isinstance(value, str) and value:
        return value
    stable = row.get("stable_source_path")
    if isinstance(stable, str) and stable:
        return stable
    family = semantic_role(row)
    line = row.get("source_line")
    suffix = f"{family}.{line}" if is_positive_int(line) else family
    return f"row.{index}.{suffix}"


def inline_source_audit(route_manifest: dict[str, Any]) -> dict[str, Any]:
    inputs = route_manifest.get("inputs", {})
    if not isinstance(inputs, dict):
        return {}
    for key in ("sourceAuditSummary", "sourceAudit"):
        value = inputs.get(key)
        if isinstance(value, dict):
            return value
    return {}


def route_counts(route_manifest: dict[str, Any], rows: list[Any]) -> dict[str, int]:
    counts: dict[str, int] = {}
    metrics = route_manifest.get("route_metrics", {})
    if isinstance(metrics, dict):
        for key, value in metrics.items():
            if is_non_negative_int(value):
                counts[key] = value

    coverage = route_manifest.get("component_family_coverage", {})
    if isinstance(coverage, dict):
        for key, value in coverage.items():
            if is_non_negative_int(value):
                counts[f"component_family_{key}"] = value

    counts["route_rows_total"] = sum(1 for row in rows if isinstance(row, dict))
    for row in rows:
        if not isinstance(row, dict):
            continue
        route = route_name(row.get("route_type"))
        counts[f"route_rows_{route}"] = counts.get(f"route_rows_{route}", 0) + 1
        if row.get("fallback_reason"):
            counts["route_rows_with_fallback_reason"] = counts.get("route_rows_with_fallback_reason", 0) + 1
        if row.get("recorder_eligibility") == "candidate":
            counts["route_rows_recorder_candidate"] = counts.get("route_rows_recorder_candidate", 0) + 1

    return counts


def primitive_counts(rows: list[Any]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for row in rows:
        if not isinstance(row, dict):
            continue
        role = semantic_role(row)
        counts[f"primitive_{role}"] = counts.get(f"primitive_{role}", 0) + 1
        if row.get("parameter_bindings"):
            counts["with_parameter_bindings"] = counts.get("with_parameter_bindings", 0) + 1
        if row.get("event_contracts"):
            counts["with_event_contracts"] = counts.get("with_event_contracts", 0) + 1
        if row.get("gesture_contracts"):
            counts["with_gesture_contracts"] = counts.get("with_gesture_contracts", 0) + 1
        if row.get("state_contracts"):
            counts["with_state_contracts"] = counts.get("with_state_contracts", 0) + 1
        if row.get("style_token_references"):
            counts["with_style_token_references"] = counts.get("with_style_token_references", 0) + 1
    return counts


def routes_from_rows(rows: list[Any]) -> list[dict[str, Any]]:
    routes = []
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            continue
        node_id = row_node_id(row, index)
        chosen = route_name(row.get("route_type"))
        fallback = row.get("fallback_reason")
        route: dict[str, Any] = {
            "node_id": node_id,
            "semantic_role": semantic_role(row),
            "chosen_route": chosen,
            "candidate_routes": [{
                "route": chosen,
                "support": "present",
                "confidence": row.get("confidence", 0.0) if is_finite_number(row.get("confidence")) else 0.0,
            }],
            "reason": f"source contract maps to {semantic_role(row)}",
            "requires_js_engine": chosen in {ROUTE_LIVE_JS, ROUTE_HYBRID},
            "requires_gpu": chosen in {ROUTE_NATIVE_CPP, ROUTE_NATIVE_HTML, ROUTE_RECORDED_PAINT, ROUTE_HYBRID},
        }
        if chosen in NATIVE_ROUTES:
            route["requires_js_engine"] = False
            route["validation_refs"] = [f"route_manifest:{node_id}"]
        if chosen in FALLBACK_ROUTES:
            route["fallback_reason"] = fallback if isinstance(fallback, str) and fallback else "requires live or unsupported behavior"
        routes.append(route)
    return routes
