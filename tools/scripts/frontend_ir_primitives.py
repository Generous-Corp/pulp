#!/usr/bin/env python3
"""Summarize primitive coverage from a PulpFrontendIR report."""

from __future__ import annotations

import argparse
import json
import pathlib
from typing import Any
from frontend_ir_common import as_dict, as_list, load_json, write_json
from frontend_ir_validation import SCHEMAS


PRIMITIVE_CATALOG: dict[str, dict[str, Any]] = {
    "button": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "chain_info_row": {"category": "audio", "expected_routes": ["native_cpp"]},
    "chain_module": {"category": "audio", "expected_routes": ["native_cpp"]},
    "checkbox": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "color_picker": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "fader": {"category": "audio", "expected_routes": ["native_cpp"]},
    "gradient_editor": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "graph": {"category": "audio", "expected_routes": ["native_cpp", "recorded_paint"]},
    "host_action": {"category": "host", "expected_routes": ["native_cpp"]},
    "image": {"category": "general", "expected_routes": ["native_html", "recorded_paint"]},
    "input": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "knob": {"category": "audio", "expected_routes": ["native_cpp"]},
    "layout": {"category": "layout", "expected_routes": ["native_html", "native_cpp"]},
    "led": {"category": "audio", "expected_routes": ["native_cpp"]},
    "list": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "meter": {"category": "audio", "expected_routes": ["native_cpp"]},
    "module_row": {"category": "audio", "expected_routes": ["native_cpp"]},
    "popover": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "radio": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "scrollbar": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "segmented_choice": {"category": "general", "expected_routes": ["native_cpp"]},
    "select": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "svg": {"category": "vector", "expected_routes": ["native_html", "recorded_paint"]},
    "text": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "text_area": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "text_input": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "toggle_button": {"category": "audio", "expected_routes": ["native_cpp"]},
    "tree": {"category": "general", "expected_routes": ["native_html", "native_cpp"]},
    "vector": {"category": "vector", "expected_routes": ["native_html", "recorded_paint"]},
    "waveform": {"category": "audio", "expected_routes": ["native_cpp", "recorded_paint"]},
    "waveform_choice": {"category": "audio", "expected_routes": ["native_cpp"]},
    "waveform_display": {"category": "audio", "expected_routes": ["native_cpp", "recorded_paint"]},
    "xy_pad": {"category": "audio", "expected_routes": ["native_cpp"]},
}


ROLE_ALIASES = {
    "body": "layout",
    "checkbox_control": "checkbox",
    "choice": "segmented_choice",
    "container": "layout",
    "dialog": "popover",
    "div": "layout",
    "h1": "text",
    "h2": "text",
    "h3": "text",
    "label": "text",
    "main": "layout",
    "meter_bar": "meter",
    "navigation": "list",
    "path": "vector",
    "section": "layout",
    "slider": "fader",
    "switch": "toggle_button",
    "tab": "segmented_choice",
    "text_button": "button",
    "text_editor": "text_input",
    "textarea": "text_area",
}


def repo_relative(path: pathlib.Path, repo_root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def normalize_role(role: Any) -> str:
    if not isinstance(role, str) or not role:
        return "unknown"
    normalized = role.strip().lower().replace("-", "_").replace(" ", "_").replace("/", "_")
    return ROLE_ALIASES.get(normalized, normalized)


def route_by_node(report: dict[str, Any]) -> dict[str, dict[str, Any]]:
    routes = {}
    for route in as_list(report.get("routes")):
        if not isinstance(route, dict):
            continue
        node_id = route.get("node_id")
        if isinstance(node_id, str) and node_id:
            routes[node_id] = route
    return routes


def style_value_count(style: dict[str, Any]) -> int:
    count = 0
    for bucket in ("layout", "typography"):
        count += len(as_dict(style.get(bucket)))
    count += len(as_list(style.get("paint_layers")))
    for variant in as_dict(style.get("variants")).values():
        count += len(as_dict(variant))
    return count


def state_summary(state: dict[str, Any]) -> dict[str, int]:
    return {
        "parameters": len(as_list(state.get("parameters"))),
        "meters": len(as_list(state.get("meters"))),
        "local_ui_keys": len(as_dict(state.get("local_ui"))),
        "derived_keys": len(as_dict(state.get("derived"))),
        "dynamic_risks": len(as_list(state.get("dynamic_risk"))),
    }


def node_evidence(node: dict[str, Any], route: dict[str, Any]) -> dict[str, Any]:
    state = state_summary(as_dict(node.get("state")))
    source_span = as_dict(node.get("source_span"))
    return {
        "id": node.get("id", ""),
        "source_role": node.get("semantic_role", ""),
        "route": route.get("chosen_route", ""),
        "requires_js_engine": route.get("requires_js_engine"),
        "source_span_present": bool(source_span.get("path")),
        "style_values": style_value_count(as_dict(node.get("style"))),
        "state": state,
        "has_binding": state["parameters"] > 0 or state["meters"] > 0 or state["local_ui_keys"] > 0,
    }


def count_by(values: list[str]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for value in values:
        counts[value] = counts.get(value, 0) + 1
    return dict(sorted(counts.items()))


def primitive_rows(report: dict[str, Any]) -> list[dict[str, Any]]:
    routes = route_by_node(report)
    grouped: dict[str, list[dict[str, Any]]] = {}
    for node in as_list(report.get("nodes")):
        if not isinstance(node, dict):
            continue
        node_id = node.get("id")
        route = routes.get(node_id, {}) if isinstance(node_id, str) else {}
        role = normalize_role(node.get("semantic_role"))
        grouped.setdefault(role, []).append(node_evidence(node, route))

    rows = []
    for role, nodes in sorted(grouped.items()):
        catalog = PRIMITIVE_CATALOG.get(role)
        category = str(catalog.get("category")) if catalog else "unknown"
        expected_routes = list(catalog.get("expected_routes", [])) if catalog else []
        route_counts = count_by([
            node["route"] for node in nodes
            if isinstance(node.get("route"), str) and node["route"]
        ])
        rows.append({
            "role": role,
            "category": category,
            "in_catalog": catalog is not None,
            "expected_routes": expected_routes,
            "observed_routes": route_counts,
            "node_count": len(nodes),
            "nodes_with_source_span": sum(1 for node in nodes if node.get("source_span_present") is True),
            "nodes_with_style": sum(1 for node in nodes if node.get("style_values", 0) > 0),
            "nodes_with_binding": sum(1 for node in nodes if node.get("has_binding") is True),
            # Fail-closed: a node is "no JS" only if its route explicitly proves
            # requires_js_engine == False. A missing/non-bool value (e.g. an
            # unrouted node) counts as JS-requiring so it cannot slip through
            # native-readiness.
            "nodes_requiring_js": sum(1 for node in nodes if node.get("requires_js_engine") is not False),
            "node_samples": nodes[:10],
        })
    return rows


def catalog_summary(rows: list[dict[str, Any]]) -> dict[str, Any]:
    observed_roles = {str(row.get("role", "")) for row in rows if row.get("role")}
    known_roles = set(PRIMITIVE_CATALOG)
    categories = count_by([
        str(row.get("category", "unknown"))
        for row in rows
    ])
    return {
        "catalog_primitives": len(known_roles),
        "observed_primitives": len(observed_roles),
        "covered_catalog_primitives": len(observed_roles & known_roles),
        "observed_missing_from_catalog": sorted(observed_roles - known_roles),
        "catalog_not_observed": sorted(known_roles - observed_roles),
        "observed_categories": categories,
    }


def build_primitive_report(
    report: dict[str, Any],
    source_path: pathlib.Path | None = None,
    repo_root: pathlib.Path | None = None,
) -> dict[str, Any]:
    root = repo_root or pathlib.Path.cwd()
    primitives = primitive_rows(report)
    summary = catalog_summary(primitives)
    validation = as_dict(report.get("validation"))
    summary["nodes"] = len(as_list(report.get("nodes")))
    summary["routes"] = len(as_list(report.get("routes")))
    # Per-node route membership, not just equal counts: a node missing a route
    # balanced by a duplicate/orphan route must still surface as uncovered.
    routed_ids = set(route_by_node(report).keys())
    summary["nodes_without_route"] = sum(
        1 for node in as_list(report.get("nodes"))
        if not (isinstance(node, dict) and isinstance(node.get("id"), str) and node.get("id") in routed_ids)
    )
    summary["nodes_with_source_span"] = sum(row["nodes_with_source_span"] for row in primitives)
    summary["nodes_with_style"] = sum(row["nodes_with_style"] for row in primitives)
    summary["nodes_with_binding"] = sum(row["nodes_with_binding"] for row in primitives)
    summary["nodes_requiring_js"] = sum(row["nodes_requiring_js"] for row in primitives)
    return {
        "schema": SCHEMAS["primitive_coverage"],
        "fixture_id": str(report.get("fixture_id", "")),
        "frontend_ir": {
            "path": repo_relative(source_path, root) if source_path else "",
        },
        "summary": summary,
        "source_counts": validation.get("source_counts", {}),
        "primitive_counts": validation.get("primitive_counts", {}),
        "primitives": primitives,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frontend-ir", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args(argv)

    report = load_json(args.frontend_ir)
    write_json(
        args.output,
        build_primitive_report(report, args.frontend_ir, args.repo_root),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
