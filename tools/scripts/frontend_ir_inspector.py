#!/usr/bin/env python3
"""Build an inspector-oriented summary from a PulpFrontendIR report."""

from __future__ import annotations

import argparse
import json
import pathlib
from typing import Any
from frontend_ir_common import as_dict, as_list, load_json, write_json
from frontend_ir_validation import SCHEMAS


def repo_relative(path: pathlib.Path, repo_root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def count_routes(routes: list[Any]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for route in routes:
        if not isinstance(route, dict):
            continue
        chosen = route.get("chosen_route")
        if isinstance(chosen, str) and chosen:
            counts[chosen] = counts.get(chosen, 0) + 1
    return counts


def route_by_node(report: dict[str, Any]) -> dict[str, dict[str, Any]]:
    routes = {}
    for route in as_list(report.get("routes")):
        if not isinstance(route, dict):
            continue
        node_id = route.get("node_id")
        if isinstance(node_id, str) and node_id:
            routes[node_id] = route
    return routes


def route_resource_usage(resources: list[Any]) -> dict[str, list[str]]:
    usage: dict[str, list[str]] = {}
    for resource in resources:
        if not isinstance(resource, dict):
            continue
        resource_id = resource.get("id")
        if not isinstance(resource_id, str) or not resource_id:
            continue
        for route_name in as_list(resource.get("route_usage")):
            if isinstance(route_name, str) and route_name:
                usage.setdefault(route_name, []).append(resource_id)
    return {
        route_name: sorted(set(resource_ids))
        for route_name, resource_ids in sorted(usage.items())
    }


def resources_requested_by_node(resources: list[Any], node_id: str, source_span: dict[str, Any]) -> list[str]:
    anchors = {node_id}
    span_node_id = source_span.get("node_id")
    if isinstance(span_node_id, str) and span_node_id:
        anchors.add(span_node_id)
    span_path = source_span.get("path")
    if isinstance(span_path, str) and span_path:
        anchors.add(span_path)

    ids = []
    for resource in resources:
        if not isinstance(resource, dict):
            continue
        resource_id = resource.get("id")
        requested_by = {
            item for item in as_list(resource.get("requested_by"))
            if isinstance(item, str) and item
        }
        if isinstance(resource_id, str) and anchors.intersection(requested_by):
            ids.append(resource_id)
    return sorted(ids)


def resource_log(report: dict[str, Any]) -> list[dict[str, Any]]:
    entries = []
    for resource in as_list(report.get("resources")):
        if not isinstance(resource, dict):
            continue
        entry = {
            "id": resource.get("id", ""),
            "origin": resource.get("original_uri", ""),
            "resolved_uri": resource.get("resolved_uri", ""),
            "mime": resource.get("mime", ""),
            "sha256": resource.get("sha256", ""),
            "byte_size": resource.get("byte_size", 0),
            "route_usage": as_list(resource.get("route_usage")),
            "requested_by": as_list(resource.get("requested_by")),
            "watch": resource.get("watch") is True,
            "transforms": as_list(resource.get("transforms")),
        }
        if "bundle_destination" in resource:
            entry["bundle_destination"] = resource["bundle_destination"]
        entries.append(entry)
    return sorted(entries, key=lambda item: str(item.get("id", "")))


def style_values(style: dict[str, Any]) -> list[dict[str, Any]]:
    values = []
    for bucket in ("layout", "typography"):
        entries = style.get(bucket, {})
        if isinstance(entries, dict):
            values.extend(value for value in entries.values() if isinstance(value, dict))
    paint_layers = style.get("paint_layers", [])
    if isinstance(paint_layers, list):
        values.extend(value for value in paint_layers if isinstance(value, dict))
    variants = style.get("variants", {})
    if isinstance(variants, dict):
        for entries in variants.values():
            if isinstance(entries, dict):
                values.extend(value for value in entries.values() if isinstance(value, dict))
    return values


def support_counts(style: dict[str, Any]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for value in style_values(style):
        support = value.get("support", {})
        if not isinstance(support, dict):
            continue
        for backend, status in support.items():
            if isinstance(backend, str) and isinstance(status, str):
                key = f"{backend}:{status}"
                counts[key] = counts.get(key, 0) + 1
    return dict(sorted(counts.items()))


def style_summary(style: dict[str, Any]) -> dict[str, Any]:
    variants = style.get("variants", {})
    variant_values = 0
    if isinstance(variants, dict):
        for entries in variants.values():
            if isinstance(entries, dict):
                variant_values += len(entries)
    return {
        "layout_values": len(as_dict(style.get("layout"))),
        "paint_layers": len(as_list(style.get("paint_layers"))),
        "typography_values": len(as_dict(style.get("typography"))),
        "variant_values": variant_values,
        "fallback_reasons": as_list(style.get("fallback_reasons")),
        "support_counts": support_counts(style),
    }


def state_summary(state: dict[str, Any]) -> dict[str, int]:
    return {
        "parameters": len(as_list(state.get("parameters"))),
        "meters": len(as_list(state.get("meters"))),
        "local_ui_keys": len(as_dict(state.get("local_ui"))),
        "derived_keys": len(as_dict(state.get("derived"))),
        "dynamic_risks": len(as_list(state.get("dynamic_risk"))),
    }


def token_summary(report: dict[str, Any]) -> dict[str, Any]:
    tokens = as_dict(report.get("tokens"))
    type_counts: dict[str, int] = {}
    resolved = 0
    for token in tokens.values():
        if not isinstance(token, dict):
            continue
        token_type = token.get("type")
        if isinstance(token_type, str) and token_type:
            type_counts[token_type] = type_counts.get(token_type, 0) + 1
        if "resolved_value" in token:
            resolved += 1
    return {
        "total": len(tokens),
        "resolved": resolved,
        "unresolved": len(tokens) - resolved,
        "types": dict(sorted(type_counts.items())),
    }


def tweak_cards_for_node(tweaks: list[Any], node_id: str) -> list[dict[str, Any]]:
    cards = []
    for tweak in tweaks:
        if not isinstance(tweak, dict) or tweak.get("node_id") != node_id:
            continue
        cards.append({
            "property": tweak.get("property", ""),
            "value": tweak.get("value"),
            "invalidates": as_list(tweak.get("invalidates")),
            "classification_preserved": tweak_preserves_classification(tweak),
        })
    return sorted(cards, key=lambda item: str(item.get("property", "")))


def tweak_preserves_classification(tweak: dict[str, Any]) -> bool:
    # Derive preservation the same way frontend_ir_session does: a tweak that
    # invalidates source or route cannot preserve component classification, no
    # matter what the producer's flag claims. The producer flag can only make a
    # non-invalidating tweak NON-preserving, never the reverse.
    invalidates = as_list(tweak.get("invalidates"))
    if any(scope in {"source", "route"} for scope in invalidates):
        return False
    return tweak.get("classification_preserved") is not False


def tweak_summary(report: dict[str, Any]) -> dict[str, Any]:
    tweaks = as_list(report.get("tweaks"))
    invalidation_counts: dict[str, int] = {}
    node_ids = set()
    preserved = 0
    route_or_source_invalidating = 0
    for tweak in tweaks:
        if not isinstance(tweak, dict):
            continue
        node_id = tweak.get("node_id")
        if isinstance(node_id, str) and node_id:
            node_ids.add(node_id)
        if tweak_preserves_classification(tweak):
            preserved += 1
        invalidates = as_list(tweak.get("invalidates"))
        if any(scope in {"source", "route"} for scope in invalidates):
            route_or_source_invalidating += 1
        for scope in invalidates:
            if isinstance(scope, str) and scope:
                invalidation_counts[scope] = invalidation_counts.get(scope, 0) + 1
    return {
        "total": len(tweaks),
        "classification_preserved": preserved,
        "source_or_route_invalidating": route_or_source_invalidating,
        "invalidations": dict(sorted(invalidation_counts.items())),
        "nodes": sorted(node_ids),
    }


def node_cards(report: dict[str, Any]) -> list[dict[str, Any]]:
    routes = route_by_node(report)
    resources = as_list(report.get("resources"))
    tweaks = as_list(report.get("tweaks"))
    cards = []
    for node in as_list(report.get("nodes")):
        if not isinstance(node, dict):
            continue
        node_id = node.get("id", "")
        route = routes.get(node_id, {})
        chosen_route = route.get("chosen_route", "")
        source_span = as_dict(node.get("source_span"))
        card = {
            "id": node_id,
            "semantic_role": node.get("semantic_role", ""),
            "source_span": source_span,
            "route": {
                "chosen_route": chosen_route,
                "requires_js_engine": route.get("requires_js_engine"),
                "requires_gpu": route.get("requires_gpu"),
                "fallback_reason": route.get("fallback_reason", ""),
                "validation_refs": as_list(route.get("validation_refs")),
            },
            "style": style_summary(as_dict(node.get("style"))),
            "state": state_summary(as_dict(node.get("state"))),
            "resources": {
                "explicit": as_list(node.get("resources")),
                "requested_by_node": resources_requested_by_node(resources, str(node_id), source_span),
            },
            "tweaks": tweak_cards_for_node(tweaks, str(node_id)),
        }
        cards.append(card)
    return sorted(cards, key=lambda item: str(item.get("id", "")))


def validation_summary(report: dict[str, Any], gates: list[dict[str, Any]],
                       session_diff: dict[str, Any] | None = None) -> dict[str, Any]:
    validation = as_dict(report.get("validation"))
    summary = {
        "compile": validation.get("compile", {}),
        "binary_dependencies": validation.get("binary_dependencies", {}),
        "screenshots": as_list(validation.get("screenshots")),
        "proofs": as_list(validation.get("proofs")),
        "gates": [
            {
                "mode": gate.get("mode", ""),
                "verdict": gate.get("verdict", ""),
                "summary": gate.get("summary", {}),
            }
            for gate in gates
        ],
    }
    if session_diff is not None:
        summary["session_diff"] = {
            "schema": session_diff.get("schema", ""),
            "summary": session_diff.get("summary", {}),
        }
    return summary


def build_inspector_report(report: dict[str, Any], gates: list[dict[str, Any]] | None = None,
                           session_diff: dict[str, Any] | None = None,
                           source_path: pathlib.Path | None = None,
                           repo_root: pathlib.Path | None = None) -> dict[str, Any]:
    gates = gates or []
    resources = as_list(report.get("resources"))
    routes = as_list(report.get("routes"))
    cards = node_cards(report)
    root = repo_root or pathlib.Path.cwd()
    source = as_dict(report.get("source"))
    tokens = token_summary(report)
    tweaks = tweak_summary(report)
    return {
        "schema": SCHEMAS["inspector"],
        "fixture_id": str(report.get("fixture_id", "")),
        "source": {
            "kind": source.get("kind", ""),
            "path": source.get("path", ""),
            "sha256": source.get("sha256", ""),
            "source_of_truth": source.get("source_of_truth", ""),
            "counts": source.get("counts", {}),
        },
        "frontend_ir": {
            "path": repo_relative(source_path, root) if source_path else "",
        },
        "summary": {
            "nodes": len(cards),
            "resources": len(resources),
            "tokens": tokens["total"],
            "tweaks": tweaks["total"],
            "watchable_resources": sum(1 for resource in resources if isinstance(resource, dict) and resource.get("watch") is True),
            "routes": count_routes(routes),
            "js_required_routes": sum(
                1 for route in routes
                if isinstance(route, dict) and route.get("requires_js_engine") is True
            ),
            "fallback_routes": sum(
                1 for route in routes
                if isinstance(route, dict) and isinstance(route.get("fallback_reason"), str) and route["fallback_reason"]
            ),
        },
        "tokens": tokens,
        "tweaks": tweaks,
        "route_resource_usage": route_resource_usage(resources),
        "resource_log": resource_log(report),
        "nodes": cards,
        "validation": validation_summary(report, gates, session_diff),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frontend-ir", required=True, type=pathlib.Path)
    parser.add_argument("--gate", action="append", type=pathlib.Path, default=[])
    parser.add_argument("--session-diff", type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args(argv)

    report = load_json(args.frontend_ir)
    gates = [load_json(path) for path in args.gate]
    session_diff = load_json(args.session_diff) if args.session_diff else None
    write_json(
        args.output,
        build_inspector_report(report, gates, session_diff, args.frontend_ir, args.repo_root),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
