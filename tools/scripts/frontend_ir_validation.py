#!/usr/bin/env python3
"""Validation helpers for PulpFrontendIR v0 reports."""

from __future__ import annotations

import math
import re
from typing import Any


ROUTE_NATIVE_CPP = "native_cpp"
ROUTE_NATIVE_HTML = "native_html"
ROUTE_RECORDED_PAINT = "recorded_paint"
ROUTE_LIVE_JS = "live_js"
ROUTE_HYBRID = "hybrid"
ROUTE_UNSUPPORTED = "unsupported"
ROUTES = {
    ROUTE_LIVE_JS,
    ROUTE_NATIVE_HTML,
    ROUTE_NATIVE_CPP,
    ROUTE_RECORDED_PAINT,
    ROUTE_HYBRID,
    ROUTE_UNSUPPORTED,
}
NATIVE_ROUTES = {ROUTE_NATIVE_HTML, ROUTE_NATIVE_CPP, ROUTE_RECORDED_PAINT}
FALLBACK_ROUTES = {ROUTE_LIVE_JS, ROUTE_HYBRID, ROUTE_UNSUPPORTED}
PLANNED_SUPPORT_ROUTES = {
    ROUTE_LIVE_JS,
    ROUTE_NATIVE_HTML,
    ROUTE_NATIVE_CPP,
    ROUTE_RECORDED_PAINT,
    ROUTE_HYBRID,
}
SOURCE_TRUTHS = {"archived_fixture", "local_file", "mcp_payload", "generated", "runtime_capture"}
SHA256_RE = re.compile(r"^[a-f0-9]{64}$")


# Canonical registry of frontend-IR schema name strings.
#
# Single source of truth for the schema identifiers emitted and checked by the
# frontend_ir producer/consumer scripts. Logical key -> exact wire string. The
# string VALUES are the contract: they are matched byte-for-byte by producers,
# consumers, and external tooling, so they must never change here without a
# coordinated schema-version bump. Add a new key rather than editing a value.
SCHEMAS = {
    # Core frontend-IR report and its acceptance gate.
    "frontend_ir": "pulp-frontend-ir-v0",
    "gate": "pulp-frontend-ir-gate-v0",
    # Primitive-coverage report and its gate.
    "primitive_coverage": "pulp-frontend-ir-primitive-coverage-v0",
    "primitive_gate": "pulp-frontend-ir-primitive-gate-v0",
    # Codegen artifacts manifest and its gate.
    "codegen_artifacts": "pulp-frontend-ir-codegen-artifacts-v0",
    "codegen_artifact_gate": "pulp-frontend-ir-codegen-artifact-gate-v0",
    # Aggregate native-validation gate verdict.
    "native_validation_gate": "pulp-frontend-ir-native-validation-gate-v0",
    # Inspector snapshot.
    "inspector": "pulp-frontend-ir-inspector-v0",
    # Session manifest and session diff.
    "session": "pulp-frontend-ir-session-v0",
    "session_diff": "pulp-frontend-ir-session-diff-v0",
    # Tweaks payloads (legacy + namespaced aliases, both accepted on read).
    "tweaks": "pulp-tweaks-v0",
    "tweaks_namespaced": "pulp-frontend-ir-tweaks-v0",
    # Native C++ binding manifest consumed by the codegen-artifacts producer.
    "native_cpp_binding_manifest": "pulp-native-cpp-binding-manifest-v1",
}


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def is_non_negative_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def is_positive_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def is_finite_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


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


def validate_token(value: Any, field: str) -> None:
    expect(isinstance(value, dict), f"{field} must be an object")
    expect(isinstance(value.get("type"), str) and bool(value["type"]), f"{field}.type must be a string")
    expect("value" in value, f"{field}.value is required")
    source_identity = value.get("source_identity")
    if source_identity is not None:
        expect(isinstance(source_identity, dict), f"{field}.source_identity must be an object")
        for key, source_value in source_identity.items():
            expect(isinstance(key, str) and bool(key), f"{field}.source_identity keys must be strings")
            expect(isinstance(source_value, str), f"{field}.source_identity.{key} must be a string")


def validate_tweak(value: Any, field: str) -> None:
    expect(isinstance(value, dict), f"{field} must be an object")
    expect(isinstance(value.get("node_id"), str) and bool(value["node_id"]), f"{field}.node_id must be a string")
    expect(isinstance(value.get("property"), str) and bool(value["property"]), f"{field}.property must be a string")
    expect("value" in value, f"{field}.value is required")
    invalidates = value.get("invalidates", [])
    expect(isinstance(invalidates, list), f"{field}.invalidates must be an array")
    for index, value_name in enumerate(invalidates):
        expect(value_name in {"source", "style", "resource", "route", "validation"},
               f"{field}.invalidates[{index}] is invalid")
    if "classification_preserved" in value:
        expect(isinstance(value["classification_preserved"], bool),
               f"{field}.classification_preserved must be a boolean")


def validate_frontend_ir(report: dict[str, Any]) -> None:
    expect(report.get("schema") == SCHEMAS["frontend_ir"], "schema must be pulp-frontend-ir-v0")
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
    if "tokens" in report:
        expect(isinstance(report["tokens"], dict), "tokens must be an object")
        for key, token in report["tokens"].items():
            expect(isinstance(key, str) and bool(key), "tokens keys must be non-empty strings")
            validate_token(token, f"tokens.{key}")
    if "tweaks" in report:
        expect(isinstance(report["tweaks"], list), "tweaks must be an array")
        for index, tweak in enumerate(report["tweaks"]):
            validate_tweak(tweak, f"tweaks[{index}]")

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
        if chosen in FALLBACK_ROUTES:
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
    if "state_counts" in validation:
        validate_count_map(validation.get("state_counts"), "validation.state_counts")
    if "token_counts" in validation:
        validate_count_map(validation.get("token_counts"), "validation.token_counts")
    if "tweak_counts" in validation:
        validate_count_map(validation.get("tweak_counts"), "validation.tweak_counts")
