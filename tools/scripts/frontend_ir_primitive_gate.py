#!/usr/bin/env python3
"""Evaluate gates for PulpFrontendIR primitive coverage reports."""

from __future__ import annotations

import argparse
import json
import pathlib
from typing import Any
from frontend_ir_common import as_dict, as_list, load_json, non_negative_int, write_json
# Import the canonical native-route set rather than re-typing the string
# literals — a local copy silently drifts when a route is added to the set.
from frontend_ir_validation import NATIVE_ROUTES


PASS_STATUS = "pass"
WARN_STATUS = "warn"
FAIL_STATUS = "fail"
READY_VERDICT = "ready"
NOT_READY_VERDICT = "not_ready"
BINDING_EXPECTED_ROLES = {
    "checkbox",
    "color_picker",
    "fader",
    "gradient_editor",
    "knob",
    "radio",
    "scrollbar",
    "select",
    "segmented_choice",
    "text_area",
    "text_input",
    "toggle_button",
    "xy_pad",
}


def check(check_id: str, status: str, message: str, **details: Any) -> dict[str, Any]:
    result: dict[str, Any] = {
        "id": check_id,
        "status": status,
        "message": message,
    }
    if details:
        result["details"] = details
    return result


def validate_primitive_coverage(report: dict[str, Any]) -> None:
    if report.get("schema") != "pulp-frontend-ir-primitive-coverage-v0":
        raise ValueError("schema must be pulp-frontend-ir-primitive-coverage-v0")
    if not isinstance(report.get("fixture_id"), str):
        raise ValueError("fixture_id must be a string")
    if not isinstance(report.get("summary"), dict):
        raise ValueError("summary must be an object")
    if not isinstance(report.get("primitives"), list):
        raise ValueError("primitives must be an array")

    summary = report["summary"]
    for key in (
        "catalog_primitives",
        "observed_primitives",
        "covered_catalog_primitives",
        "nodes",
        "routes",
        "nodes_with_source_span",
        "nodes_with_style",
        "nodes_with_binding",
        "nodes_requiring_js",
    ):
        if not isinstance(summary.get(key), int) or isinstance(summary.get(key), bool) or summary[key] < 0:
            raise ValueError(f"summary.{key} must be a non-negative integer")
    for key in ("observed_missing_from_catalog", "catalog_not_observed"):
        if not isinstance(summary.get(key), list):
            raise ValueError(f"summary.{key} must be an array")

    for index, row in enumerate(report["primitives"]):
        if not isinstance(row, dict):
            raise ValueError(f"primitives[{index}] must be an object")
        if not isinstance(row.get("role"), str) or not row["role"]:
            raise ValueError(f"primitives[{index}].role must be a non-empty string")
        if not isinstance(row.get("in_catalog"), bool):
            raise ValueError(f"primitives[{index}].in_catalog must be a boolean")
        for key in (
            "node_count",
            "nodes_with_source_span",
            "nodes_with_style",
            "nodes_with_binding",
            "nodes_requiring_js",
        ):
            if not isinstance(row.get(key), int) or isinstance(row.get(key), bool) or row[key] < 0:
                raise ValueError(f"primitives[{index}].{key} must be a non-negative integer")
        if not isinstance(row.get("expected_routes"), list):
            raise ValueError(f"primitives[{index}].expected_routes must be an array")
        if not isinstance(row.get("observed_routes"), dict):
            raise ValueError(f"primitives[{index}].observed_routes must be an object")


def unexpected_route_rows(primitives: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for row in primitives:
        expected_routes = {str(route) for route in as_list(row.get("expected_routes"))}
        observed_routes = {
            str(route): non_negative_int(count)
            for route, count in as_dict(row.get("observed_routes")).items()
        }
        unexpected = {
            route: count
            for route, count in observed_routes.items()
            if route and count > 0 and route not in expected_routes
        }
        if unexpected:
            rows.append({
                "role": row.get("role", ""),
                "expected_routes": sorted(expected_routes),
                "unexpected_routes": unexpected,
            })
    return rows


def binding_warning_rows(primitives: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for row in primitives:
        role = str(row.get("role", ""))
        node_count = non_negative_int(row.get("node_count"))
        with_binding = non_negative_int(row.get("nodes_with_binding"))
        if role in BINDING_EXPECTED_ROLES and node_count > 0 and with_binding == 0:
            rows.append({"role": role, "node_count": node_count})
    return rows


def coverage_checks(report: dict[str, Any]) -> list[dict[str, Any]]:
    checks: list[dict[str, Any]] = []
    try:
        validate_primitive_coverage(report)
    except ValueError as exc:
        checks.append(check("schema_valid", FAIL_STATUS, str(exc)))
        return checks

    checks.append(check("schema_valid", PASS_STATUS, "primitive coverage schema validation passed"))
    summary = as_dict(report.get("summary"))
    primitives = [row for row in as_list(report.get("primitives")) if isinstance(row, dict)]
    nodes = non_negative_int(summary.get("nodes"))
    routes = non_negative_int(summary.get("routes"))
    observed = non_negative_int(summary.get("observed_primitives"))

    if observed == 0 or not primitives:
        checks.append(check("primitive_rows_present", FAIL_STATUS, "report has no primitive rows"))
    else:
        checks.append(check(
            "primitive_rows_present",
            PASS_STATUS,
            "primitive rows are present",
            observed_primitives=observed,
        ))

    missing = [str(item) for item in as_list(summary.get("observed_missing_from_catalog")) if item]
    if missing:
        checks.append(check(
            "no_unknown_primitives",
            FAIL_STATUS,
            "observed primitive roles are missing from the Pulp catalog",
            missing=missing,
        ))
    else:
        checks.append(check("no_unknown_primitives", PASS_STATUS, "all observed primitive roles are cataloged"))

    nodes_without_route = non_negative_int(summary.get("nodes_without_route"))
    if nodes == 0:
        checks.append(check("route_coverage", FAIL_STATUS, "report has no nodes"))
    elif nodes_without_route > 0 or routes != nodes:
        checks.append(check(
            "route_coverage",
            FAIL_STATUS,
            "not every node has route coverage",
            nodes=nodes,
            routes=routes,
            nodes_without_route=nodes_without_route,
        ))
    else:
        checks.append(check("route_coverage", PASS_STATUS, "every node has route coverage", nodes=nodes))

    with_span = non_negative_int(summary.get("nodes_with_source_span"))
    if nodes == 0:
        pass
    elif with_span != nodes:
        checks.append(check(
            "source_span_coverage",
            FAIL_STATUS,
            "not every primitive node has source-span evidence",
            nodes=nodes,
            nodes_with_source_span=with_span,
        ))
    else:
        checks.append(check("source_span_coverage", PASS_STATUS, "every primitive node has source-span evidence"))

    with_style = non_negative_int(summary.get("nodes_with_style"))
    if nodes > 0 and with_style == 0:
        checks.append(check("style_evidence", WARN_STATUS, "primitive nodes have no style evidence"))
    else:
        checks.append(check(
            "style_evidence",
            PASS_STATUS,
            "primitive style evidence is present",
            nodes_with_style=with_style,
        ))

    js_nodes = non_negative_int(summary.get("nodes_requiring_js"))
    if js_nodes:
        checks.append(check(
            "js_requirement",
            WARN_STATUS,
            "some primitive nodes still require a JS-backed route",
            nodes_requiring_js=js_nodes,
        ))
    else:
        checks.append(check("js_requirement", PASS_STATUS, "no primitive nodes require JS"))

    route_rows = unexpected_route_rows(primitives)
    if route_rows:
        checks.append(check(
            "expected_route_alignment",
            WARN_STATUS,
            "some primitives use routes outside the current catalog expectation",
            rows=route_rows[:10],
            total=len(route_rows),
        ))
    else:
        checks.append(check("expected_route_alignment", PASS_STATUS, "observed primitive routes match catalog expectations"))

    binding_rows = binding_warning_rows(primitives)
    if binding_rows:
        checks.append(check(
            "binding_evidence",
            WARN_STATUS,
            "some interactive primitives have no binding evidence",
            rows=binding_rows[:10],
            total=len(binding_rows),
        ))
    else:
        checks.append(check("binding_evidence", PASS_STATUS, "interactive primitive binding evidence is present or not required"))

    return checks


def native_readiness_checks(report: dict[str, Any]) -> list[dict[str, Any]]:
    checks = coverage_checks(report)
    if any(item["status"] == FAIL_STATUS for item in checks):
        return checks

    summary = as_dict(report.get("summary"))
    primitives = [row for row in as_list(report.get("primitives")) if isinstance(row, dict)]
    js_nodes = non_negative_int(summary.get("nodes_requiring_js"))
    if js_nodes:
        checks.append(check(
            "native_no_js_primitives",
            FAIL_STATUS,
            "primitive native-readiness cannot include JS-required nodes",
            nodes_requiring_js=js_nodes,
        ))
    else:
        checks.append(check("native_no_js_primitives", PASS_STATUS, "all primitive nodes are no-runtime-JS routes"))

    non_native_rows = []
    for row in primitives:
        observed_routes = as_dict(row.get("observed_routes"))
        non_native = {
            str(route): non_negative_int(count)
            for route, count in observed_routes.items()
            if str(route) not in NATIVE_ROUTES and non_negative_int(count) > 0
        }
        if non_native:
            non_native_rows.append({"role": row.get("role", ""), "routes": non_native})
    if non_native_rows:
        checks.append(check(
            "native_routes_only",
            FAIL_STATUS,
            "primitive native-readiness requires native routes only",
            rows=non_native_rows[:10],
            total=len(non_native_rows),
        ))
    else:
        checks.append(check("native_routes_only", PASS_STATUS, "all primitive routes are native-capable"))

    return checks


def gate_primitive_coverage(report: dict[str, Any], mode: str) -> dict[str, Any]:
    if mode == "coverage":
        checks = coverage_checks(report)
    elif mode == "native-readiness":
        checks = native_readiness_checks(report)
    else:
        raise ValueError(f"unknown gate mode: {mode}")

    failures = sum(1 for item in checks if item["status"] == FAIL_STATUS)
    warnings = sum(1 for item in checks if item["status"] == WARN_STATUS)
    return {
        "schema": "pulp-frontend-ir-primitive-gate-v0",
        "fixture_id": str(report.get("fixture_id", "")),
        "mode": mode,
        "verdict": READY_VERDICT if failures == 0 else NOT_READY_VERDICT,
        "summary": {
            "checks": len(checks),
            "failures": failures,
            "warnings": warnings,
        },
        "checks": checks,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--primitive-coverage", required=True, type=pathlib.Path)
    parser.add_argument("--mode", choices=("coverage", "native-readiness"), default="coverage")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--allow-not-ready", action="store_true",
                        help="write the gate report but return success even when verdict is not_ready")
    args = parser.parse_args(argv)

    report = load_json(args.primitive_coverage)
    gate = gate_primitive_coverage(report, args.mode)
    if args.output:
        write_json(args.output, gate)
    else:
        print(json.dumps(gate, indent=2, sort_keys=True))
    if gate["verdict"] != READY_VERDICT and not args.allow_not_ready:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
