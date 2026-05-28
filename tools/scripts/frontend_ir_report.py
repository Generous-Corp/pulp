#!/usr/bin/env python3
"""Build a PulpFrontendIR v0 report from existing import validation artifacts."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import re
from typing import Any


ROUTE_NATIVE_CPP = "native_cpp"
ROUTE_LIVE_JS = "live_js"
ROUTE_HYBRID = "hybrid"
ROUTE_UNSUPPORTED = "unsupported"
ROUTES = {ROUTE_LIVE_JS, "native_html", ROUTE_NATIVE_CPP, "recorded_paint", ROUTE_HYBRID, ROUTE_UNSUPPORTED}
NATIVE_ROUTES = {"native_html", ROUTE_NATIVE_CPP, "recorded_paint"}
SOURCE_TRUTHS = {"archived_fixture", "local_file", "mcp_payload", "generated", "runtime_capture"}
SHA256_RE = re.compile(r"^[a-f0-9]{64}$")


def load_json(path: pathlib.Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return data


def write_json(path: pathlib.Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate_sha(value: Any, field: str) -> None:
    if value is None:
        return
    expect(isinstance(value, str) and bool(SHA256_RE.fullmatch(value)), f"{field} must be a lowercase sha256")


def validate_count_map(value: Any, field: str) -> None:
    expect(isinstance(value, dict), f"{field} must be an object")
    for key, count in value.items():
        expect(isinstance(key, str) and bool(key), f"{field} keys must be non-empty strings")
        expect(is_non_negative_int(count), f"{field}.{key} must be a non-negative integer")


def validate_artifact_ref(value: Any, field: str) -> None:
    expect(isinstance(value, dict), f"{field} must be an object")
    expect(isinstance(value.get("path"), str), f"{field}.path must be a string")
    validate_sha(value.get("sha256"), f"{field}.sha256")


def validate_source_span(value: Any, field: str) -> None:
    expect(isinstance(value, dict), f"{field} must be an object")
    expect(isinstance(value.get("node_id"), str) and bool(value["node_id"]), f"{field}.node_id must be a string")
    expect(isinstance(value.get("path"), str) and bool(value["path"]), f"{field}.path must be a string")
    for key in ("line", "column", "end_line", "end_column"):
        if key in value:
            expect(is_positive_int(value[key]), f"{field}.{key} must be a positive integer")


def validate_resource(value: Any, field: str) -> None:
    expect(isinstance(value, dict), f"{field} must be an object")
    expect(isinstance(value.get("id"), str) and bool(value["id"]), f"{field}.id must be a string")
    expect(isinstance(value.get("original_uri"), str) and bool(value["original_uri"]),
           f"{field}.original_uri must be a string")
    expect(isinstance(value.get("route_usage"), list), f"{field}.route_usage must be an array")
    for index, route in enumerate(value["route_usage"]):
        expect(route in ROUTES, f"{field}.route_usage[{index}] is invalid")
    validate_sha(value.get("sha256"), f"{field}.sha256")
    if "byte_size" in value:
        expect(is_non_negative_int(value["byte_size"]), f"{field}.byte_size must be a non-negative integer")


def validate_frontend_ir(report: dict[str, Any]) -> None:
    expect(report.get("schema") == "pulp-frontend-ir-v0", "schema must be pulp-frontend-ir-v0")
    for key in ("source", "design_ir", "nodes", "routes", "validation"):
        expect(key in report, f"missing required field: {key}")

    source = report["source"]
    expect(isinstance(source, dict), "source must be an object")
    expect(source.get("kind") in {"jsx", "html", "design_json", "runtime_snapshot", "pulp_js"}, "source.kind is invalid")
    expect(isinstance(source.get("path"), str), "source.path must be a string")
    expect(source.get("source_of_truth") in SOURCE_TRUTHS, "source.source_of_truth is invalid")
    validate_sha(source.get("sha256"), "source.sha256")
    validate_count_map(source.get("counts"), "source.counts")
    if "spans" in source:
        expect(isinstance(source["spans"], list), "source.spans must be an array")
        for index, span in enumerate(source["spans"]):
            validate_source_span(span, f"source.spans[{index}]")

    validate_artifact_ref(report["design_ir"], "design_ir")
    if "route_manifest" in report:
        validate_artifact_ref(report["route_manifest"], "route_manifest")

    expect(isinstance(report["nodes"], list), "nodes must be an array")
    for index, node in enumerate(report["nodes"]):
        expect(isinstance(node, dict), f"nodes[{index}] must be an object")
        expect(isinstance(node.get("id"), str) and bool(node["id"]), f"nodes[{index}].id is required")
        expect(isinstance(node.get("semantic_role"), str) and bool(node["semantic_role"]),
               f"nodes[{index}].semantic_role is required")
        expect(isinstance(node.get("style"), dict), f"nodes[{index}].style must be an object")
        expect(isinstance(node.get("state"), dict), f"nodes[{index}].state must be an object")
        if "source_span" in node:
            validate_source_span(node["source_span"], f"nodes[{index}].source_span")

    if "resources" in report:
        expect(isinstance(report["resources"], list), "resources must be an array")
        for index, resource in enumerate(report["resources"]):
            validate_resource(resource, f"resources[{index}]")

    expect(isinstance(report["routes"], list), "routes must be an array")
    for index, route in enumerate(report["routes"]):
        expect(isinstance(route, dict), f"routes[{index}] must be an object")
        chosen = route.get("chosen_route")
        expect(chosen in ROUTES, f"routes[{index}].chosen_route is invalid")
        expect(isinstance(route.get("node_id"), str) and bool(route["node_id"]), f"routes[{index}].node_id is required")
        expect(isinstance(route.get("reason"), str) and bool(route["reason"]), f"routes[{index}].reason is required")
        if chosen in NATIVE_ROUTES:
            expect(route.get("requires_js_engine") is False,
                   f"routes[{index}] native route must set requires_js_engine=false")
            expect(isinstance(route.get("validation_refs"), list) and bool(route["validation_refs"]),
                   f"routes[{index}] native route must include validation_refs")
        if chosen in {ROUTE_LIVE_JS, ROUTE_HYBRID, ROUTE_UNSUPPORTED}:
            expect(isinstance(route.get("fallback_reason"), str) and bool(route["fallback_reason"]),
                   f"routes[{index}] fallback route must include fallback_reason")

    validation = report["validation"]
    expect(isinstance(validation, dict), "validation must be an object")
    validate_count_map(validation.get("source_counts"), "validation.source_counts")
    validate_count_map(validation.get("style_counts"), "validation.style_counts")
    if "route_counts" in validation:
        validate_count_map(validation.get("route_counts"), "validation.route_counts")
    if "primitive_counts" in validation:
        validate_count_map(validation.get("primitive_counts"), "validation.primitive_counts")
    if "resource_counts" in validation:
        validate_count_map(validation.get("resource_counts"), "validation.resource_counts")


def repo_relative(path: pathlib.Path, repo_root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def is_non_negative_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def is_positive_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def is_finite_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def route_name(value: str | None) -> str:
    normalized = (value or "").strip().lower().replace("-", "_")
    if normalized in {ROUTE_NATIVE_CPP, "native", "cpp", "native_host_service"}:
        return ROUTE_NATIVE_CPP
    if normalized in {ROUTE_LIVE_JS, "live", "runtime_js"}:
        return ROUTE_LIVE_JS
    if normalized in {"native_html", "native_layout", "native_tree", "native_element"}:
        return "native_html"
    if normalized == ROUTE_HYBRID:
        return ROUTE_HYBRID
    if normalized in {"recorded", "recorded_paint", "native_custom_paint"}:
        return "recorded_paint"
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
    fallback_rows = overlay.get("route_rows")
    if isinstance(fallback_rows, list):
        return fallback_rows
    return rows if isinstance(rows, list) else []


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


def metric_key(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]+", "_", value).strip("_").lower() or "unknown"


def resource_id_key(value: str) -> str:
    return metric_key(re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", value))


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
            "map_calls",
            "ternaries",
            "set_param_calls",
            "style_objects",
            "css_values",
            "css_values_valid",
            "css_values_invalid",
            "svg_vector_nodes",
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


def route_usage_for_input(key: str, rows: list[Any]) -> list[str]:
    normalized = resource_id_key(key)
    if normalized in {"bundle", "runtime_trace"}:
        return [ROUTE_LIVE_JS]
    if normalized in {"materialized_audit", "cpp", "native_audit"}:
        return [ROUTE_NATIVE_CPP]
    if normalized in {"ir", "source_jsx", "source_audit"}:
        return routes_present(rows)
    return routes_present(rows)


def path_byte_size(path: str, repo_root: pathlib.Path) -> int | None:
    candidate = pathlib.Path(path)
    if not candidate.is_absolute():
        candidate = repo_root / candidate
    try:
        if candidate.is_file():
            return candidate.stat().st_size
    except OSError:
        return None
    return None


def input_resource(key: str, value: Any, rows: list[Any], repo_root: pathlib.Path,
                   requested_by: str) -> dict[str, Any] | None:
    path_value = ""
    sha = ""
    if isinstance(value, dict):
        path = value.get("path")
        if isinstance(path, str) and path:
            path_value = path
        candidate_sha = value.get("sha256")
        if isinstance(candidate_sha, str):
            sha = candidate_sha
    elif isinstance(value, str) and value and ("/" in value or "." in pathlib.Path(value).name):
        path_value = value

    if not path_value:
        return None

    resource: dict[str, Any] = {
        "id": f"input.{resource_id_key(key)}",
        "original_uri": path_value,
        "resolved_uri": path_value,
        "requested_by": [requested_by],
        "route_usage": route_usage_for_input(key, rows),
        "transforms": [],
        "watch": key in {"sourceJsx", "bundle", "sourceAudit"},
    }
    if sha:
        resource["sha256"] = sha
    byte_size = path_byte_size(path_value, repo_root)
    if byte_size is not None:
        resource["byte_size"] = byte_size
    return resource


def resources_from_manifest(route_manifest: dict[str, Any], rows: list[Any], repo_root: pathlib.Path) -> list[dict[str, Any]]:
    inputs = route_manifest.get("inputs", {})
    if not isinstance(inputs, dict):
        return []
    requested_by = str(route_manifest.get("fixture") or route_manifest.get("schema") or "route_manifest")
    resources = []
    for key, value in inputs.items():
        resource = input_resource(key, value, rows, repo_root, requested_by)
        if resource is not None:
            resources.append(resource)
    return resources


def resource_counts(resources: list[dict[str, Any]]) -> dict[str, int]:
    counts = {
        "total": len(resources),
        "with_sha256": 0,
        "with_byte_size": 0,
        "watchable": 0,
    }
    for resource in resources:
        if resource.get("sha256"):
            counts["with_sha256"] += 1
        if is_non_negative_int(resource.get("byte_size")):
            counts["with_byte_size"] += 1
        if resource.get("watch") is True:
            counts["watchable"] += 1
        for route in resource.get("route_usage", []) or []:
            if isinstance(route, str):
                counts[f"route_usage_{route}"] = counts.get(f"route_usage_{route}", 0) + 1
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
    support = {
        "live_js": "planned",
        "native_html": "planned",
        "native_cpp": "planned",
        "recorded_paint": "planned",
        "hybrid": "planned",
    }
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
        parameters.append({
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
        })

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
            "requires_gpu": chosen in {ROUTE_NATIVE_CPP, "native_html", "recorded_paint", ROUTE_HYBRID},
        }
        if chosen in {ROUTE_NATIVE_CPP, "native_html", "recorded_paint"}:
            route["requires_js_engine"] = False
            route["validation_refs"] = [f"route_manifest:{node_id}"]
        if chosen in {ROUTE_LIVE_JS, ROUTE_HYBRID, ROUTE_UNSUPPORTED}:
            route["fallback_reason"] = fallback if isinstance(fallback, str) and fallback else "requires live or unsupported behavior"
        routes.append(route)
    return routes


def style_counts(nodes: list[dict[str, Any]]) -> dict[str, int]:
    supported = 0
    unsupported = 0
    for node in nodes:
        style = node.get("style", {})
        if not isinstance(style, dict):
            continue
        layout = style.get("layout", {})
        if isinstance(layout, dict):
            supported += len(layout)
        paint = style.get("paint_layers", [])
        if isinstance(paint, list):
            supported += len(paint)
        fallback = style.get("fallback_reasons", [])
        if isinstance(fallback, list):
            unsupported += len(fallback)
    return {
        "supported": supported,
        "unsupported": unsupported,
    }


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


def build_frontend_ir(
    route_manifest: dict[str, Any],
    source_audit: dict[str, Any],
    route_manifest_path: pathlib.Path,
    repo_root: pathlib.Path,
) -> dict[str, Any]:
    overlay = route_manifest.get("source_contract_overlay", {})
    if not isinstance(overlay, dict):
        overlay = {}
    rows = route_rows(route_manifest)
    if not source_audit:
        source_audit = inline_source_audit(route_manifest)

    source_jsx = route_manifest.get("inputs", {}).get("sourceJsx", {})
    if not isinstance(source_jsx, dict):
        source_jsx = {}
    source_path = source_jsx.get("path") or source_audit.get("input") or ""
    if not isinstance(source_path, str):
        source_path = ""

    counts = count_map(source_audit, rows)
    nodes = nodes_from_rows(rows)
    resources = resources_from_manifest(route_manifest, rows, repo_root)
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
        "tokens": {},
        "tweaks": [],
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
            "style_counts": style_counts(nodes),
            "route_counts": route_counts(route_manifest, rows),
            "primitive_counts": primitive_counts(rows),
            "resource_counts": resource_counts(resources),
            "compile": {
                "status": "not_run",
            },
            "binary_dependencies": {
                "js_engine_present": bool(route_manifest.get("route_metrics", {}).get("js_engine_initialized", False)),
            },
            "screenshots": [],
            "notes": notes,
        },
    }
    sha = source_jsx.get("sha256")
    if isinstance(sha, str) and sha:
        report["source"]["sha256"] = sha
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--route-manifest", required=True, type=pathlib.Path)
    parser.add_argument("--source-audit", type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args(argv)

    route_manifest = load_json(args.route_manifest)
    source_audit = load_json(args.source_audit) if args.source_audit else {}
    report = build_frontend_ir(route_manifest, source_audit, args.route_manifest, args.repo_root)
    validate_frontend_ir(report)
    write_json(args.output, report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
