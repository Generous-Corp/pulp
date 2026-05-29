#!/usr/bin/env python3
"""Build a PulpFrontendIR v0 report from existing import validation artifacts."""

from __future__ import annotations

import argparse
import json
import pathlib
from typing import Any

from frontend_ir_proofs import apply_native_proofs, load_native_proof
from frontend_ir_resources import resource_counts, resources_from_manifest
from frontend_ir_sources import metric_key, source_input
from frontend_ir_tokens import resolve_source_token_refs
from frontend_ir_tweaks import tweaks_from_sidecar
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
    validate_count_map,
    validate_frontend_ir,
)
from frontend_ir_common import load_json, write_json


def repo_relative(path: pathlib.Path, repo_root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


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


def count_map(source_audit: dict[str, Any], rows: list[Any] | None = None) -> dict[str, int]:
    counts: dict[str, int] = {}
    for key in ("lines", "bytes"):
        value = source_audit.get(key)
        if is_non_negative_int(value):
            counts[key] = value

    input_summary = source_audit.get("input", {})
    if isinstance(input_summary, dict):
        value = input_summary.get("bytes")
        if is_non_negative_int(value):
            counts.setdefault("bytes", value)

    jsx_summary = source_audit.get("summary", {})
    if isinstance(jsx_summary, dict):
        for key in (
            "parse_errors",
            "jsx_elements",
            "html_elements",
            "map_calls",
            "ternaries",
            "set_param_calls",
            "class_attributes",
            "class_names",
            "css_rules",
            "style_objects",
            "style_attributes",
            "inline_style_attributes",
            "inline_style_values",
            "css_values",
            "css_values_valid",
            "css_values_invalid",
            "svg_vector_nodes",
            "stylesheet_links",
            "local_stylesheet_resources",
            "image_assets",
            "local_image_resources",
            "native_candidate_components",
            "standard_source_component_instances",
            "expanded_native_candidate_instances",
            "expanded_choice_instances",
        ):
            value = jsx_summary.get(key)
            if is_non_negative_int(value):
                counts[key] = value
        component_counts = jsx_summary.get("component_counts", {})
        if isinstance(component_counts, dict):
            for key, value in component_counts.items():
                if isinstance(key, str) and is_non_negative_int(value):
                    counts[f"component_{metric_key(key)}"] = value
        state_setters = jsx_summary.get("state_setters", {})
        if isinstance(state_setters, dict):
            counts["state_setters"] = len(state_setters)
        arrays = jsx_summary.get("arrays", {})
        if isinstance(arrays, dict):
            counts["array_constants"] = len(arrays)

    materiality = source_audit.get("materiality", {})
    if isinstance(materiality, dict):
        for key, value in materiality.items():
            if isinstance(key, str) and is_non_negative_int(value):
                counts[f"materiality_{metric_key(key)}"] = value

    template_counts = source_audit.get("sourceTemplateCounts", {})
    if isinstance(template_counts, dict):
        for key, value in template_counts.items():
            if isinstance(key, str) and is_non_negative_int(value):
                counts[key] = value

    component_invocations = source_audit.get("componentInvocationTemplates", {})
    if isinstance(component_invocations, dict):
        counts["component_invocations"] = sum(
            value for value in component_invocations.values() if is_non_negative_int(value)
        )

    component_props = source_audit.get("componentProps", {})
    if isinstance(component_props, dict):
        counts["component_prop_names"] = sum(
            len(value) for value in component_props.values() if isinstance(value, list)
        )

    list_count_keys = {
        "componentInvocationProps": "component_invocation_rows",
        "bindings": "binding_rows",
        "styleKeys": "style_keys",
        "parameterStateKeys": "parameter_state_keys",
    }
    for source_key, count_key in list_count_keys.items():
        value = source_audit.get(source_key)
        if isinstance(value, list):
            counts[count_key] = len(value)

    for dict_key, prefix in (
        ("expandedRuntimeSurface", "runtime_surface"),
        ("expandedSvgEstimate", "expanded_svg"),
    ):
        values = source_audit.get(dict_key, {})
        if isinstance(values, dict):
            for key, value in values.items():
                if isinstance(key, str) and is_non_negative_int(value):
                    counts[f"{prefix}_{key}"] = value

    if rows is not None:
        contract_rows = [row for row in rows if isinstance(row, dict)]
        counts["source_contract_rows"] = len(contract_rows)
        counts["source_contract_rows_with_source_span"] = sum(
            1 for row in contract_rows if isinstance(row.get("stable_source_path"), str) and row["stable_source_path"]
        )
        counts["source_contract_state_contracts"] = sum(
            len(row.get("state_contracts", []))
            for row in contract_rows
            if isinstance(row.get("state_contracts"), list)
        )

    return counts


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


def token_key(value: str) -> str:
    return value.strip()


def tokens_from_rows(rows: list[Any]) -> dict[str, dict[str, Any]]:
    tokens: dict[str, dict[str, Any]] = {}
    for row in rows:
        if not isinstance(row, dict):
            continue
        for token in row.get("style_token_references", []) or []:
            if not isinstance(token, str):
                continue
            key = token_key(token)
            if not key:
                continue
            tokens.setdefault(key, {
                "type": "reference",
                "value": key,
                "source_identity": {
                    "provenance": "style_token_references",
                    "source": "route_manifest",
                },
            })
    return dict(sorted(tokens.items()))


def token_counts(tokens: dict[str, dict[str, Any]], rows: list[Any]) -> dict[str, int]:
    counts = {
        "total": len(tokens),
        "unresolved": 0,
        "referenced_by_rows": 0,
    }
    referenced_rows = set()
    for token in tokens.values():
        if "resolved_value" not in token:
            counts["unresolved"] += 1
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            continue
        refs = [
            token for token in row.get("style_token_references", []) or []
            if isinstance(token, str) and token_key(token)
        ]
        if refs:
            referenced_rows.add(index)
    counts["referenced_by_rows"] = len(referenced_rows)
    return counts


def tweak_counts(tweaks: list[dict[str, Any]]) -> dict[str, int]:
    counts = {
        "total": len(tweaks),
        "classification_preserved": 0,
    }
    for tweak in tweaks:
        if not isinstance(tweak, dict):
            continue
        if tweak.get("classification_preserved") is True:
            counts["classification_preserved"] += 1
        for invalidation in tweak.get("invalidates", []) or []:
            if isinstance(invalidation, str):
                counts[f"invalidates_{metric_key(invalidation)}"] = counts.get(
                    f"invalidates_{metric_key(invalidation)}", 0
                ) + 1
    return counts


def dynamic_risks(counts: dict[str, int]) -> list[str]:
    risks: list[str] = []
    if counts.get("useState", 0) > 0:
        risks.append("react_state_hooks")
    if counts.get("useEffect", 0) > 0:
        risks.append("react_effects")
    if counts.get("useRef", 0) > 0:
        risks.append("react_refs")
    if counts.get("useCallback", 0) > 0:
        risks.append("react_callbacks")
    if counts.get("mapCalls", 0) > 0 or counts.get("map_calls", 0) > 0:
        risks.append("runtime_array_maps")
    return risks


def source_span(row: dict[str, Any], node_id: str | None = None) -> dict[str, Any] | None:
    path = row.get("stable_source_path")
    if not isinstance(path, str) or not path:
        return None
    span: dict[str, Any] = {
        "node_id": node_id or str(row.get("id", "")),
        "path": path,
    }
    line = row.get("source_line")
    if is_positive_int(line):
        span["line"] = line
    return span


def source_spans(source_audit: dict[str, Any], rows: list[Any], source_path: str) -> list[dict[str, Any]]:
    spans: list[dict[str, Any]] = []
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            continue
        span = source_span(row, row_node_id(row, index))
        if span:
            spans.append(span)

    for source_key, prefix in (
        ("componentInvocationProps", "component"),
        ("bindings", "binding"),
    ):
        if not source_path:
            break
        entries = source_audit.get(source_key, [])
        if not isinstance(entries, list):
            continue
        for index, entry in enumerate(entries):
            if not isinstance(entry, dict):
                continue
            line = entry.get("line")
            if not is_positive_int(line):
                continue
            label = entry.get("name") or entry.get("param") or prefix
            if not isinstance(label, str):
                label = prefix
            spans.append({
                "node_id": f"{prefix}.{index}.{label}",
                "path": source_path,
                "line": line,
                "label": label,
            })

    return spans


def support_for_route(route: str) -> dict[str, str]:
    support = {name: "planned" for name in sorted(PLANNED_SUPPORT_ROUTES)}
    if route in support:
        support[route] = "present"
    return support


def style_for_row(row: dict[str, Any]) -> dict[str, Any]:
    layout: dict[str, Any] = {}
    if is_finite_number(row.get("size")):
        layout["size"] = {
            "value": row["size"],
            "unit": "px",
            "provenance": "inline_style",
            "support": support_for_route(route_name(row.get("route_type"))),
        }

    paint_layers = []
    for token in row.get("style_token_references", []) or []:
        if isinstance(token, str):
            paint_layers.append({
                "value": token,
                "provenance": "token",
                "support": support_for_route(route_name(row.get("route_type"))),
            })

    style: dict[str, Any] = {
        "layout": layout,
        "paint_layers": paint_layers,
        "typography": {},
        "variants": {},
    }
    fallback = row.get("fallback_reason")
    if isinstance(fallback, str) and fallback:
        style["fallback_reasons"] = [fallback]
    return style


def state_for_row(row: dict[str, Any]) -> dict[str, Any]:
    parameters = []
    for binding in row.get("parameter_bindings", []) or []:
        if not isinstance(binding, dict):
            continue
        param_id = binding.get("param_key") or binding.get("binding_contract_id")
        if not isinstance(param_id, str) or not param_id:
            continue
        parameter: dict[str, Any] = {
            "id": param_id,
            "kind": "parameter",
            "gesture_policy": ",".join(
                boundary
                for gesture in row.get("gesture_contracts", []) or []
                if isinstance(gesture, dict)
                for boundary in gesture.get("boundaries", []) or []
                if isinstance(boundary, str)
            ),
            "route_usage": [route_name(row.get("route_type"))],
        }
        for source_key, target_key in (
            ("binding_contract_id", "source_binding_id"),
            ("module", "module"),
            ("param", "param"),
        ):
            value = binding.get(source_key)
            if isinstance(value, str) and value:
                parameter[target_key] = value
        for source_key, target_key in (
            ("value", "value"),
            ("initial_value", "initial_value"),
        ):
            value = row.get(source_key)
            if is_finite_number(value):
                parameter[target_key] = float(value)
        default_value = row.get("default_value")
        if is_finite_number(default_value):
            parameter["range"] = {"default": float(default_value)}
        default_source = row.get("default_value_source")
        if isinstance(default_source, str) and default_source:
            parameter["default_source"] = default_source
        parameters.append(parameter)

    derived = {}
    label = row.get("label")
    if isinstance(label, str) and label:
        derived["label"] = label

    local_ui = {}
    for contract in row.get("state_contracts", []) or []:
        if not isinstance(contract, dict):
            continue
        state_key = contract.get("state_key")
        if not isinstance(state_key, str) or not state_key:
            continue
        kind = contract.get("kind")
        local_ui[state_key] = kind if isinstance(kind, str) and kind else "state"

    return {
        "parameters": parameters,
        "meters": [],
        "local_ui": local_ui,
        "derived": derived,
        "dynamic_risk": [],
    }


def semantic_role(row: dict[str, Any]) -> str:
    primitive = row.get("required_native_primitive")
    if isinstance(primitive, str) and primitive:
        return primitive
    family = row.get("source_component_family") or row.get("source_component_name")
    if isinstance(family, str) and family:
        return family.strip().lower().replace(" ", "_")
    return "unknown"


def nodes_from_rows(rows: list[Any]) -> list[dict[str, Any]]:
    nodes = []
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            continue
        node_id = row_node_id(row, index)
        node: dict[str, Any] = {
            "id": node_id,
            "semantic_role": semantic_role(row),
            "style": style_for_row(row),
            "state": state_for_row(row),
            "resources": [],
        }
        span = source_span(row, node_id)
        if span:
            node["source_span"] = span
        nodes.append(node)
    return nodes


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


def iter_style_values(style: dict[str, Any]) -> list[dict[str, Any]]:
    values = []
    for bucket in ("layout", "typography"):
        entries = style.get(bucket, {})
        if isinstance(entries, dict):
            values.extend(value for value in entries.values() if isinstance(value, dict))

    paint = style.get("paint_layers", [])
    if isinstance(paint, list):
        values.extend(value for value in paint if isinstance(value, dict))

    variants = style.get("variants", {})
    if isinstance(variants, dict):
        for entries in variants.values():
            if isinstance(entries, dict):
                values.extend(value for value in entries.values() if isinstance(value, dict))
    return values


def style_counts(nodes: list[dict[str, Any]], source_counts: dict[str, int] | None = None) -> dict[str, int]:
    supported = 0
    unsupported = 0
    support_counts: dict[str, int] = {}
    for node in nodes:
        style = node.get("style", {})
        if not isinstance(style, dict):
            continue
        values = iter_style_values(style)
        supported += len(values)
        for value in values:
            support = value.get("support", {})
            if not isinstance(support, dict):
                continue
            for backend, status in support.items():
                if not isinstance(backend, str) or not isinstance(status, str):
                    continue
                key = f"support_{metric_key(backend)}_{metric_key(status)}"
                support_counts[key] = support_counts.get(key, 0) + 1
        fallback = style.get("fallback_reasons", [])
        if isinstance(fallback, list):
            unsupported += len(fallback)
    counts = {
        "supported": supported,
        "unsupported": unsupported,
    }
    counts.update(dict(sorted(support_counts.items())))
    if source_counts:
        for source_keys, style_key in (
            (("css_values",), "source_css_values"),
            (("css_values_valid",), "source_css_values_valid_syntax"),
            (("css_values_invalid",), "source_css_values_invalid_syntax"),
            (("style_objects", "inlineStyleObjects"), "source_style_objects"),
            (("styleAttributes", "style_attributes", "inline_style_attributes"), "source_style_attributes"),
            (("style_keys",), "source_style_keys"),
            (("materiality_style_values_normalized",), "source_style_values_normalized"),
            (("materiality_css_lexer_matches",), "source_css_lexer_matches"),
            (("materiality_dynamic_style_values",), "source_dynamic_style_values"),
            (("materiality_conditional_style_values",), "source_conditional_style_values"),
        ):
            for source_key in source_keys:
                value = source_counts.get(source_key)
                if is_non_negative_int(value):
                    counts[style_key] = value
                    break
    return counts


def state_counts(nodes: list[dict[str, Any]]) -> dict[str, int]:
    counts = {
        "parameters": 0,
        "parameters_with_value": 0,
        "parameters_with_initial_value": 0,
        "parameters_with_default": 0,
        "parameters_with_source_binding_id": 0,
        "parameters_with_module_param": 0,
        "meters": 0,
        "local_ui_state_keys": 0,
    }
    for node in nodes:
        state = node.get("state", {})
        if not isinstance(state, dict):
            continue
        parameters = state.get("parameters", [])
        if isinstance(parameters, list):
            counts["parameters"] += len(parameters)
            for parameter in parameters:
                if not isinstance(parameter, dict):
                    continue
                if is_finite_number(parameter.get("value")):
                    counts["parameters_with_value"] += 1
                if is_finite_number(parameter.get("initial_value")):
                    counts["parameters_with_initial_value"] += 1
                if isinstance(parameter.get("range"), dict) and is_finite_number(parameter["range"].get("default")):
                    counts["parameters_with_default"] += 1
                if isinstance(parameter.get("source_binding_id"), str) and parameter["source_binding_id"]:
                    counts["parameters_with_source_binding_id"] += 1
                if (isinstance(parameter.get("module"), str) and parameter["module"] and
                        isinstance(parameter.get("param"), str) and parameter["param"]):
                    counts["parameters_with_module_param"] += 1
        meters = state.get("meters", [])
        if isinstance(meters, list):
            counts["meters"] += len(meters)
        local_ui = state.get("local_ui", {})
        if isinstance(local_ui, dict):
            counts["local_ui_state_keys"] += len(local_ui)
    return counts


def artifact_ref_from_manifest(route_manifest: dict[str, Any], key: str, kind: str) -> dict[str, Any] | None:
    value = route_manifest.get("inputs", {}).get(key)
    if not isinstance(value, dict):
        return None
    path = value.get("path")
    if not isinstance(path, str) or not path:
        return None
    ref = {
        "path": path,
        "kind": kind,
    }
    sha = value.get("sha256")
    if isinstance(sha, str) and sha:
        ref["sha256"] = sha
    return ref


def binary_dependency_evidence(route_manifest: dict[str, Any]) -> dict[str, Any]:
    metrics = route_manifest.get("route_metrics", {})
    if not isinstance(metrics, dict):
        return {}
    js_initialized = metrics.get("js_engine_initialized")
    if js_initialized is True:
        return {
            "js_engine_present": True,
            "source": "route_manifest_runtime_metrics",
        }
    return {}


def build_frontend_ir(
    route_manifest: dict[str, Any],
    source_audit: dict[str, Any],
    route_manifest_path: pathlib.Path,
    repo_root: pathlib.Path,
    tweaks: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    overlay = route_manifest.get("source_contract_overlay", {})
    if not isinstance(overlay, dict):
        overlay = {}
    rows = route_rows(route_manifest)
    if not source_audit:
        source_audit = inline_source_audit(route_manifest)

    source_artifact = source_input(route_manifest)
    source_path = source_artifact.get("path") or source_audit.get("input") or ""
    if not isinstance(source_path, str):
        source_path = ""

    counts = count_map(source_audit, rows)
    nodes = nodes_from_rows(rows)
    resources = resources_from_manifest(route_manifest, routes_present(rows), repo_root)
    tokens = tokens_from_rows(rows)
    resolved_tokens = resolve_source_token_refs(tokens, source_path, repo_root)
    tweaks = tweaks or []
    source_of_truth = overlay.get("source", {}).get("source_of_truth")
    source_of_truth = normalize_source_of_truth(source_of_truth)

    design_ref = artifact_ref_from_manifest(route_manifest, "ir", "design_ir") or {
        "path": "",
        "kind": "design_ir",
    }
    design_ref.setdefault("schema", "pulp-design-ir-v1")
    notes = [
        "frontend-ir v0 wraps existing import evidence; compile and binary audits must be supplied by route validators before production gating."
    ]
    if not design_ref.get("path"):
        notes.append("route manifest did not provide a DesignIR artifact; this report covers source and route evidence only.")
    if resolved_tokens:
        notes.append(f"resolved {resolved_tokens} token references from static source token objects.")
    if tweaks:
        notes.append(f"attached {len(tweaks)} tweak sidecar edits without mutating imported source.")

    report: dict[str, Any] = {
        "schema": "pulp-frontend-ir-v0",
        "fixture_id": str(route_manifest.get("fixture") or overlay.get("fixture_id") or ""),
        "source": {
            "kind": source_kind(source_path),
            "path": source_path,
            "source_of_truth": source_of_truth,
            "counts": counts,
            "dynamic_risks": dynamic_risks(counts),
            "spans": source_spans(source_audit, rows, source_path),
        },
        "design_ir": design_ref,
        "route_manifest": {
            "path": repo_relative(route_manifest_path, repo_root),
            "schema": str(route_manifest.get("schema", "")),
            "kind": "route_manifest",
        },
        "nodes": nodes,
        "resources": resources,
        "tokens": tokens,
        "tweaks": tweaks,
        "routes": routes_from_rows(rows),
        "host": {
            "dpi_policy": "responsive",
            "input": ["pointer", "keyboard", "focus", "text_input"],
            "state_bridge": ["parameters", "gestures", "meters"],
            "surface": {
                "required_backend": "any",
            },
        },
        "validation": {
            "source_counts": counts,
            "style_counts": style_counts(nodes, counts),
            "state_counts": state_counts(nodes),
            "route_counts": route_counts(route_manifest, rows),
            "primitive_counts": primitive_counts(rows),
            "resource_counts": resource_counts(resources),
            "token_counts": token_counts(tokens, rows),
            "tweak_counts": tweak_counts(tweaks),
            "compile": {
                "status": "not_run",
            },
            "binary_dependencies": binary_dependency_evidence(route_manifest),
            "screenshots": [],
            "notes": notes,
        },
    }
    sha = source_artifact.get("sha256")
    if isinstance(sha, str) and sha:
        report["source"]["sha256"] = sha
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--route-manifest", required=True, type=pathlib.Path)
    parser.add_argument("--source-audit", type=pathlib.Path)
    parser.add_argument("--native-proof", action="append", type=pathlib.Path, default=[],
                        help="native compile/linkage proof artifact to attach to validation evidence")
    parser.add_argument("--tweaks", type=pathlib.Path,
                        help="pulp-tweaks-v0 sidecar to attach as non-source-mutating retheme evidence")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args(argv)

    route_manifest = load_json(args.route_manifest)
    source_audit = load_json(args.source_audit) if args.source_audit else {}
    rows = route_rows(route_manifest)
    valid_node_ids = {
        row_node_id(row, index)
        for index, row in enumerate(rows)
        if isinstance(row, dict)
    }
    tweaks = tweaks_from_sidecar(args.tweaks, valid_node_ids) if args.tweaks else []
    report = build_frontend_ir(route_manifest, source_audit, args.route_manifest, args.repo_root, tweaks)
    if args.native_proof:
        apply_native_proofs(report, [load_native_proof(path) for path in args.native_proof], args.repo_root)
    validate_frontend_ir(report)
    write_json(args.output, report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
