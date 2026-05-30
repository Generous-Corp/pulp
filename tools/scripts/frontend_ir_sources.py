#!/usr/bin/env python3
"""Source artifact helpers for PulpFrontendIR report assembly."""

from __future__ import annotations

import pathlib
import re
from typing import Any

from frontend_ir_routes import row_node_id
from frontend_ir_validation import is_non_negative_int, is_positive_int


SOURCE_INPUT_KEYS = ("sourceJsx", "sourceHtml", "sourceFile")
WATCH_INPUT_KEYS = frozenset((
    *SOURCE_INPUT_KEYS,
    "assets",
    "bundle",
    "fonts",
    "sourceAudit",
    "sourceCss",
    "styleSheets",
    "tokens",
    "tweaks",
))


def metric_key(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]+", "_", value).strip("_").lower() or "unknown"


def resource_id_key(value: str) -> str:
    return metric_key(re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", value))


def artifact_from_input(value: Any) -> dict[str, str]:
    artifact: dict[str, str] = {}
    if isinstance(value, dict):
        path = value.get("path")
        if isinstance(path, str) and path:
            artifact["path"] = path
        sha = value.get("sha256")
        if isinstance(sha, str) and sha:
            artifact["sha256"] = sha
        return artifact
    if isinstance(value, str) and value and ("/" in value or "." in pathlib.Path(value).name):
        artifact["path"] = value
    return artifact


def artifacts_from_input(value: Any) -> list[dict[str, str]]:
    if isinstance(value, list):
        artifacts: list[dict[str, str]] = []
        for item in value:
            artifact = artifact_from_input(item)
            if artifact.get("path"):
                artifacts.append(artifact)
        return artifacts
    artifact = artifact_from_input(value)
    return [artifact] if artifact.get("path") else []


def source_input(route_manifest: dict[str, Any]) -> dict[str, str]:
    inputs = route_manifest.get("inputs", {})
    if not isinstance(inputs, dict):
        return {}
    for key in SOURCE_INPUT_KEYS:
        artifact = artifact_from_input(inputs.get(key))
        if artifact.get("path"):
            return artifact
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
