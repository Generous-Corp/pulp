#!/usr/bin/env python3
"""Evaluate readiness gates for PulpFrontendIR v0 reports."""

from __future__ import annotations

import argparse
import json
import pathlib
from typing import Any

from frontend_ir_validation import (
    FALLBACK_ROUTES,
    ROUTE_HYBRID,
    ROUTE_LIVE_JS,
    ROUTE_UNSUPPORTED,
    SCHEMAS,
    validate_frontend_ir,
)
from frontend_ir_common import load_json, write_json


PASS_STATUS = "pass"
WARN_STATUS = "warn"
FAIL_STATUS = "fail"
READY_VERDICT = "ready"
NOT_READY_VERDICT = "not_ready"
COMPILE_PASS_STATUSES = {"pass", "passed", "ok", "success"}
SOURCE_STYLE_KEYS = (
    "source_css_values",
    "source_css_values_valid_syntax",
    "source_style_objects",
    "source_style_attributes",
    "source_style_keys",
    "source_style_values_normalized",
    "source_css_lexer_matches",
)


def check(check_id: str, status: str, message: str, **details: Any) -> dict[str, Any]:
    result: dict[str, Any] = {
        "id": check_id,
        "status": status,
        "message": message,
    }
    if details:
        result["details"] = details
    return result


def count(validation: dict[str, Any], bucket: str, key: str) -> int:
    values = validation.get(bucket, {})
    if not isinstance(values, dict):
        return 0
    value = values.get(key)
    return value if isinstance(value, int) and not isinstance(value, bool) and value >= 0 else 0


def evidence_checks(report: dict[str, Any]) -> list[dict[str, Any]]:
    checks: list[dict[str, Any]] = []
    try:
        validate_frontend_ir(report)
    except ValueError as exc:
        checks.append(check("schema_valid", FAIL_STATUS, str(exc)))
        return checks

    checks.append(check("schema_valid", PASS_STATUS, "PulpFrontendIR schema validation passed"))
    validation = report.get("validation", {})
    routes = report.get("routes", [])
    source_rows = count(validation, "source_counts", "source_contract_rows")
    source_rows_with_span = count(validation, "source_counts", "source_contract_rows_with_source_span")

    if not routes:
        checks.append(check("route_evidence_present", FAIL_STATUS, "report has no route evidence"))
    elif source_rows and len(routes) != source_rows:
        checks.append(check(
            "route_evidence_present",
            FAIL_STATUS,
            "route count does not match source contract row count",
            routes=len(routes),
            source_contract_rows=source_rows,
        ))
    else:
        checks.append(check(
            "route_evidence_present",
            PASS_STATUS,
            "route evidence is present",
            routes=len(routes),
            source_contract_rows=source_rows,
        ))

    if source_rows == 0:
        checks.append(check("source_span_coverage", WARN_STATUS, "source contract rows were not reported"))
    elif source_rows_with_span != source_rows:
        checks.append(check(
            "source_span_coverage",
            FAIL_STATUS,
            "not every source contract row has source-span evidence",
            source_contract_rows=source_rows,
            with_source_span=source_rows_with_span,
        ))
    else:
        checks.append(check(
            "source_span_coverage",
            PASS_STATUS,
            "all source contract rows have source-span evidence",
            source_contract_rows=source_rows,
        ))

    resource_total = count(validation, "resource_counts", "total")
    with_sha = count(validation, "resource_counts", "with_sha256")
    with_bytes = count(validation, "resource_counts", "with_byte_size")
    if resource_total == 0:
        checks.append(check("resource_byte_hash_coverage", WARN_STATUS, "report has no ResourceIR entries"))
    elif with_sha != resource_total or with_bytes != resource_total:
        checks.append(check(
            "resource_byte_hash_coverage",
            FAIL_STATUS,
            "not every ResourceIR entry has hash and byte-size evidence",
            total=resource_total,
            with_sha256=with_sha,
            with_byte_size=with_bytes,
        ))
    else:
        checks.append(check(
            "resource_byte_hash_coverage",
            PASS_STATUS,
            "all ResourceIR entries have hash and byte-size evidence",
            total=resource_total,
        ))

    style_supported = count(validation, "style_counts", "supported")
    source_style_total = sum(count(validation, "style_counts", key) for key in SOURCE_STYLE_KEYS)
    if style_supported == 0 and source_style_total == 0:
        checks.append(check("style_evidence_present", WARN_STATUS, "report has no source or route style evidence"))
    else:
        checks.append(check(
            "style_evidence_present",
            PASS_STATUS,
            "style evidence is present",
            supported_style_values=style_supported,
            source_style_values=source_style_total,
        ))

    return checks


def refs_real_artifact(value: Any) -> bool:
    """A proof/audit artifact must reference an actual file, not just be an
    empty dict. Otherwise `js_engine_present: false` plus a bare `{}` would
    satisfy native-readiness with no evidence behind it."""
    return isinstance(value, dict) and isinstance(value.get("path"), str) and bool(value.get("path"))


def native_readiness_checks(report: dict[str, Any]) -> list[dict[str, Any]]:
    checks = evidence_checks(report)
    if any(item["status"] == FAIL_STATUS for item in checks):
        return checks

    validation = report.get("validation", {})
    routes = report.get("routes", [])
    js_routes = [
        route.get("node_id", "")
        for route in routes
        if route.get("requires_js_engine") is True or route.get("chosen_route") in {
            ROUTE_LIVE_JS,
            ROUTE_HYBRID,
            ROUTE_UNSUPPORTED,
        }
    ]
    if js_routes:
        checks.append(check(
            "native_routes_no_js",
            FAIL_STATUS,
            "native readiness cannot include live, hybrid, unsupported, or JS-required routes",
            route_count=len(js_routes),
            sample=js_routes[:5],
        ))
    else:
        checks.append(check("native_routes_no_js", PASS_STATUS, "all routes are no-runtime-JS routes"))

    binary_dependencies = validation.get("binary_dependencies", {})
    if not isinstance(binary_dependencies, dict):
        binary_dependencies = {}
    js_present = binary_dependencies.get("js_engine_present")
    has_binary_proof = (
        refs_real_artifact(binary_dependencies.get("proof_artifact")) or
        refs_real_artifact(binary_dependencies.get("audit_artifact"))
    )
    if js_present is False and has_binary_proof:
        checks.append(check("binary_no_js_engine", PASS_STATUS, "binary dependency proof reports no JS engine"))
    else:
        checks.append(check(
            "binary_no_js_engine",
            FAIL_STATUS,
            "native readiness requires proof-backed binary evidence that no JS engine is present",
            js_engine_present=js_present,
            proof_artifact_present=has_binary_proof,
        ))

    compile_status = str(validation.get("compile", {}).get("status", "")).lower()
    if compile_status in COMPILE_PASS_STATUSES:
        checks.append(check("native_compile_pass", PASS_STATUS, "native compile evidence passed"))
    else:
        checks.append(check(
            "native_compile_pass",
            FAIL_STATUS,
            "native readiness requires passing compile evidence",
            compile_status=compile_status or "missing",
        ))

    unresolved_tokens = count(validation, "token_counts", "unresolved")
    if unresolved_tokens > 0:
        checks.append(check(
            "token_resolution",
            FAIL_STATUS,
            "native readiness requires token references to be resolved or explicitly defaulted",
            unresolved=unresolved_tokens,
        ))
    else:
        checks.append(check("token_resolution", PASS_STATUS, "no unresolved token references"))

    tweaks = report.get("tweaks", [])
    unsafe_tweaks = []
    if isinstance(tweaks, list):
        for tweak in tweaks:
            if not isinstance(tweak, dict):
                continue
            invalidates = tweak.get("invalidates", [])
            invalidates_route_or_source = (
                isinstance(invalidates, list) and
                any(scope in {"source", "route"} for scope in invalidates)
            )
            if tweak.get("classification_preserved") is False or invalidates_route_or_source:
                unsafe_tweaks.append(tweak.get("node_id", ""))
    if unsafe_tweaks:
        checks.append(check(
            "tweaks_preserve_classification",
            FAIL_STATUS,
            "native readiness cannot include tweaks that invalidate source or route classification",
            tweak_count=len(unsafe_tweaks),
            sample=unsafe_tweaks[:5],
        ))
    else:
        checks.append(check("tweaks_preserve_classification", PASS_STATUS, "tweaks preserve source and route classification"))

    unsupported_style = count(validation, "style_counts", "unsupported")
    fallback_routes = [
        route.get("node_id", "")
        for route in routes
        if route.get("chosen_route") in FALLBACK_ROUTES
    ]
    if unsupported_style or fallback_routes:
        checks.append(check(
            "no_fallback_or_unsupported_style",
            FAIL_STATUS,
            "native readiness cannot rely on fallback routes or unsupported style evidence",
            unsupported_style=unsupported_style,
            fallback_route_count=len(fallback_routes),
        ))
    else:
        checks.append(check("no_fallback_or_unsupported_style", PASS_STATUS, "no fallback route or unsupported style evidence"))

    return checks


def gate_frontend_ir(report: dict[str, Any], mode: str) -> dict[str, Any]:
    if mode == "evidence":
        checks = evidence_checks(report)
    elif mode == "native-readiness":
        checks = native_readiness_checks(report)
    else:
        raise ValueError(f"unknown gate mode: {mode}")

    failures = sum(1 for item in checks if item["status"] == FAIL_STATUS)
    warnings = sum(1 for item in checks if item["status"] == WARN_STATUS)
    verdict = READY_VERDICT if failures == 0 else NOT_READY_VERDICT
    return {
        "schema": SCHEMAS["gate"],
        "fixture_id": str(report.get("fixture_id", "")),
        "mode": mode,
        "verdict": verdict,
        "summary": {
            "checks": len(checks),
            "failures": failures,
            "warnings": warnings,
        },
        "checks": checks,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frontend-ir", required=True, type=pathlib.Path)
    parser.add_argument("--mode", choices=("evidence", "native-readiness"), default="evidence")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--allow-not-ready", action="store_true",
                        help="write the gate report but return success even when verdict is not_ready")
    args = parser.parse_args(argv)

    report = load_json(args.frontend_ir)
    gate = gate_frontend_ir(report, args.mode)
    if args.output:
        write_json(args.output, gate)
    else:
        print(json.dumps(gate, indent=2, sort_keys=True))
    if gate["verdict"] != READY_VERDICT and not args.allow_not_ready:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
