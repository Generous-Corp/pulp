#!/usr/bin/env python3
"""Evaluate gates for PulpFrontendIR generated C++ artifact reports."""

from __future__ import annotations

import argparse
import json
import pathlib
from typing import Any
from frontend_ir_common import as_dict, as_list, load_json, non_negative_int, write_json
from frontend_ir_validation import SCHEMAS


PASS_STATUS = "pass"
WARN_STATUS = "warn"
FAIL_STATUS = "fail"
READY_VERDICT = "ready"
NOT_READY_VERDICT = "not_ready"
CODEGEN_ARTIFACT_SCHEMA = SCHEMAS["codegen_artifacts"]
GATE_SCHEMA = SCHEMAS["codegen_artifact_gate"]
NATIVE_CPP_ROUTE = "native_cpp"
PROVENANCE_EXPLICIT = "explicit_source_route_id"
PROVENANCE_PREFIX = "id_prefix"
SPLIT_PROVENANCE_VALUES = {PROVENANCE_EXPLICIT, PROVENANCE_PREFIX}


def check(check_id: str, status: str, message: str, **details: Any) -> dict[str, Any]:
    result: dict[str, Any] = {
        "id": check_id,
        "status": status,
        "message": message,
    }
    if details:
        result["details"] = details
    return result


def validate_artifact_ref(value: Any, field: str) -> None:
    if not isinstance(value, dict):
        raise ValueError(f"{field} must be an object")
    if not isinstance(value.get("path"), str) or not value["path"]:
        raise ValueError(f"{field}.path must be a non-empty string")
    if not isinstance(value.get("kind"), str) or not value["kind"]:
        raise ValueError(f"{field}.kind must be a non-empty string")
    if not isinstance(value.get("sha256"), str) or len(value["sha256"]) != 64:
        raise ValueError(f"{field}.sha256 must be a sha256 string")
    if not isinstance(value.get("byte_size"), int) or isinstance(value.get("byte_size"), bool) or value["byte_size"] <= 0:
        raise ValueError(f"{field}.byte_size must be a positive integer")


def validate_codegen_artifacts(report: dict[str, Any]) -> None:
    if report.get("schema") != CODEGEN_ARTIFACT_SCHEMA:
        raise ValueError(f"schema must be {CODEGEN_ARTIFACT_SCHEMA}")
    if not isinstance(report.get("fixture_id"), str):
        raise ValueError("fixture_id must be a string")
    validate_artifact_ref(report.get("frontend_ir"), "frontend_ir")

    artifacts = report.get("artifacts")
    if not isinstance(artifacts, dict):
        raise ValueError("artifacts must be an object")
    for key in ("source_cpp", "header", "binding_manifest"):
        validate_artifact_ref(artifacts.get(key), f"artifacts.{key}")

    summary = report.get("summary")
    if not isinstance(summary, dict):
        raise ValueError("summary must be an object")
    for key in (
        "native_cpp_routes",
        "binding_entries",
        "directly_bound_native_routes",
        "missing_native_route_bindings",
        "extra_binding_entries",
        "split_binding_candidates",
    ):
        if not isinstance(summary.get(key), int) or isinstance(summary.get(key), bool) or summary[key] < 0:
            raise ValueError(f"summary.{key} must be a non-negative integer")
    for key in ("binding_primitives", "binding_route_types"):
        if not isinstance(summary.get(key), dict):
            raise ValueError(f"summary.{key} must be an object")
    for key in ("missing_native_route_bindings", "extra_binding_entries", "split_binding_candidates"):
        if not isinstance(report.get(key), list):
            raise ValueError(f"{key} must be an array")
    if summary["missing_native_route_bindings"] != len(report["missing_native_route_bindings"]):
        raise ValueError("summary.missing_native_route_bindings must match missing_native_route_bindings length")
    if summary["extra_binding_entries"] != len(report["extra_binding_entries"]):
        raise ValueError("summary.extra_binding_entries must match extra_binding_entries length")
    if summary["split_binding_candidates"] != len(report["split_binding_candidates"]):
        raise ValueError("summary.split_binding_candidates must match split_binding_candidates length")
    if summary["directly_bound_native_routes"] + summary["missing_native_route_bindings"] != summary["native_cpp_routes"]:
        raise ValueError("summary.directly_bound_native_routes plus summary.missing_native_route_bindings must equal summary.native_cpp_routes")

    # Optional one-to-many provenance breakdown (explicit_source_route_id vs
    # id_prefix). When present, validate the per-row shape and that the two
    # provenance counts partition split_binding_candidates.
    has_explicit_count = "explicit_split_bindings" in summary
    has_prefix_count = "prefix_split_bindings" in summary
    if has_explicit_count != has_prefix_count:
        raise ValueError(
            "summary.explicit_split_bindings and summary.prefix_split_bindings must both be present or both absent"
        )
    for key in ("explicit_split_bindings", "prefix_split_bindings"):
        if key in summary and (
            not isinstance(summary.get(key), int)
            or isinstance(summary.get(key), bool)
            or summary[key] < 0
        ):
            raise ValueError(f"summary.{key} must be a non-negative integer")

    explicit_rows = 0
    prefix_rows = 0
    for row in report["split_binding_candidates"]:
        if not isinstance(row, dict):
            raise ValueError("split_binding_candidates entries must be objects")
        provenance = row.get("provenance")
        if provenance is not None:
            if provenance not in SPLIT_PROVENANCE_VALUES:
                raise ValueError(
                    "split_binding_candidates.provenance must be one of "
                    f"{sorted(SPLIT_PROVENANCE_VALUES)}"
                )
            if provenance == PROVENANCE_EXPLICIT:
                explicit_rows += 1
            else:
                prefix_rows += 1
        parts = row.get("parts")
        if parts is not None:
            if not isinstance(parts, list):
                raise ValueError("split_binding_candidates.parts must be an array")
            for part in parts:
                if not isinstance(part, dict):
                    raise ValueError("split_binding_candidates.parts entries must be objects")

    if has_explicit_count:
        if summary["explicit_split_bindings"] != explicit_rows:
            raise ValueError(
                "summary.explicit_split_bindings must match the count of explicit_source_route_id split candidates"
            )
        if summary["prefix_split_bindings"] != prefix_rows:
            raise ValueError(
                "summary.prefix_split_bindings must match the count of id_prefix split candidates"
            )
        if summary["explicit_split_bindings"] + summary["prefix_split_bindings"] != summary["split_binding_candidates"]:
            raise ValueError(
                "summary.explicit_split_bindings plus summary.prefix_split_bindings must equal summary.split_binding_candidates"
            )


def split_route_ids(report: dict[str, Any]) -> set[str]:
    ids: set[str] = set()
    for row in as_list(report.get("split_binding_candidates")):
        if not isinstance(row, dict):
            continue
        route_id = row.get("route_id")
        bindings = row.get("binding_entry_ids")
        if isinstance(route_id, str) and route_id and isinstance(bindings, list) and bindings:
            ids.add(route_id)
    return ids


def split_candidate_rows(report: dict[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for row in as_list(report.get("split_binding_candidates")):
        if not isinstance(row, dict):
            continue
        route_id = row.get("route_id")
        bindings = row.get("binding_entry_ids")
        if isinstance(route_id, str) and route_id and isinstance(bindings, list) and bindings:
            rows.append(row)
    return rows


def coverage_checks(report: dict[str, Any]) -> list[dict[str, Any]]:
    checks: list[dict[str, Any]] = []
    try:
        validate_codegen_artifacts(report)
    except ValueError as exc:
        checks.append(check("schema_valid", FAIL_STATUS, str(exc)))
        return checks

    checks.append(check("schema_valid", PASS_STATUS, "codegen artifact schema validation passed"))
    summary = as_dict(report.get("summary"))
    native_routes = non_negative_int(summary.get("native_cpp_routes"))
    binding_entries = non_negative_int(summary.get("binding_entries"))
    direct_routes = non_negative_int(summary.get("directly_bound_native_routes"))
    missing_routes = [str(item) for item in as_list(report.get("missing_native_route_bindings")) if item]
    extra_entries = [str(item) for item in as_list(report.get("extra_binding_entries")) if item]
    accounted_splits = split_route_ids(report)
    unaccounted_missing = sorted(route for route in missing_routes if route not in accounted_splits)

    if native_routes == 0:
        checks.append(check("native_route_evidence", FAIL_STATUS, "report has no native C++ route evidence"))
    else:
        checks.append(check(
            "native_route_evidence",
            PASS_STATUS,
            "native C++ route evidence is present",
            native_cpp_routes=native_routes,
        ))

    if native_routes > 0 and binding_entries == 0:
        checks.append(check("binding_entries_present", FAIL_STATUS, "native C++ routes have no generated binding entries"))
    else:
        checks.append(check(
            "binding_entries_present",
            PASS_STATUS,
            "generated binding entries are present",
            binding_entries=binding_entries,
        ))

    if unaccounted_missing:
        checks.append(check(
            "unaccounted_native_route_bindings",
            FAIL_STATUS,
            "some native C++ routes have no direct or split binding evidence",
            missing=unaccounted_missing,
        ))
    else:
        checks.append(check(
            "unaccounted_native_route_bindings",
            PASS_STATUS,
            "all missing direct native route bindings are accounted for",
        ))

    split_rows = split_candidate_rows(report)
    extra_entry_set = set(extra_entries)
    explicit_rows = [row for row in split_rows if row.get("provenance") == PROVENANCE_EXPLICIT]
    prefix_only_rows = [row for row in split_rows if row.get("provenance") == PROVENANCE_PREFIX]

    # explicit_one_to_many_bindings: every binding id named by an explicit
    # parent-child link must be a real generated sub-binding (present in
    # extra_binding_entries). A phantom reference means a broken parent-child
    # link and fails the gate.
    broken_links: list[dict[str, Any]] = []
    for row in explicit_rows:
        missing_children = [
            entry_id
            for entry_id in as_list(row.get("binding_entry_ids"))
            if isinstance(entry_id, str) and entry_id not in extra_entry_set
        ]
        if missing_children:
            broken_links.append({
                "route_id": row.get("route_id"),
                "missing_binding_entry_ids": missing_children,
            })
    if broken_links:
        checks.append(check(
            "explicit_one_to_many_bindings",
            FAIL_STATUS,
            "explicit one-to-many bindings reference binding ids not present in generated sub-bindings",
            broken_links=broken_links,
        ))
    elif explicit_rows:
        checks.append(check(
            "explicit_one_to_many_bindings",
            PASS_STATUS,
            "all explicit one-to-many bindings reference real generated sub-bindings",
            explicit_split_bindings=len(explicit_rows),
        ))
    else:
        checks.append(check(
            "explicit_one_to_many_bindings",
            PASS_STATUS,
            "no explicit one-to-many bindings",
        ))

    # prefix_only_split_bindings: surface (but do not fail) missing routes that
    # are accounted for ONLY by id-prefix convention, to encourage migration to
    # explicit source_route_id modeling.
    if prefix_only_rows:
        checks.append(check(
            "prefix_only_split_bindings",
            WARN_STATUS,
            "some native C++ routes are accounted for only by id-prefix convention; prefer explicit source_route_id",
            route_ids=sorted(
                str(row.get("route_id")) for row in prefix_only_rows if row.get("route_id")
            ),
        ))
    else:
        checks.append(check(
            "prefix_only_split_bindings",
            PASS_STATUS,
            "no prefix-only split bindings",
        ))

    if direct_routes < native_routes:
        checks.append(check(
            "direct_binding_coverage",
            WARN_STATUS,
            "some native C++ routes are not directly represented by binding ids",
            native_cpp_routes=native_routes,
            directly_bound_native_routes=direct_routes,
            split_binding_candidates=len(accounted_splits),
        ))
    else:
        checks.append(check("direct_binding_coverage", PASS_STATUS, "all native C++ routes have direct binding ids"))

    if extra_entries:
        checks.append(check(
            "extra_binding_entries",
            WARN_STATUS,
            "generated binding manifest has entries outside native C++ route ids",
            entries=extra_entries[:10],
            total=len(extra_entries),
        ))
    else:
        checks.append(check("extra_binding_entries", PASS_STATUS, "no extra generated binding entries"))

    route_types = {
        str(route): non_negative_int(count)
        for route, count in as_dict(summary.get("binding_route_types")).items()
        if route and non_negative_int(count) > 0
    }
    non_native_cpp = {route: count for route, count in route_types.items() if route != NATIVE_CPP_ROUTE}
    if non_native_cpp:
        checks.append(check(
            "non_native_cpp_binding_route_types",
            WARN_STATUS,
            "generated binding manifest includes non-native-C++ route types",
            route_types=non_native_cpp,
        ))
    else:
        checks.append(check("non_native_cpp_binding_route_types", PASS_STATUS, "all binding entries are native C++ route type"))

    return checks


def gate_codegen_artifacts(report: dict[str, Any]) -> dict[str, Any]:
    checks = coverage_checks(report)
    failures = sum(1 for item in checks if item["status"] == FAIL_STATUS)
    warnings = sum(1 for item in checks if item["status"] == WARN_STATUS)
    return {
        "schema": GATE_SCHEMA,
        "fixture_id": str(report.get("fixture_id", "")),
        "mode": "coverage",
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
    parser.add_argument("--codegen-artifacts", required=True, type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--allow-not-ready", action="store_true",
                        help="write the gate report but return success even when verdict is not_ready")
    args = parser.parse_args(argv)

    gate = gate_codegen_artifacts(load_json(args.codegen_artifacts))
    if args.output:
        write_json(args.output, gate)
    else:
        print(json.dumps(gate, indent=2, sort_keys=True))
    if gate["verdict"] != READY_VERDICT and not args.allow_not_ready:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
