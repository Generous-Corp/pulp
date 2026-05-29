#!/usr/bin/env python3
"""Attach native build proof artifacts to PulpFrontendIR reports."""

from __future__ import annotations

import json
import pathlib
from typing import Any

from frontend_ir_validation import NATIVE_ROUTES, is_non_negative_int


PHASE_G_CPP_ONLY_SCHEMA = "pulp-native-ui-phase-g-cpp-only-audit-v1"
PHASE_H_COMPILE_PROBE_SCHEMA = "pulp-native-ui-phase-h-import-cpp-compile-probe-v1"


def load_native_proof(path: pathlib.Path) -> tuple[pathlib.Path, dict[str, Any]]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return path, data


def repo_relative(path: pathlib.Path, repo_root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def append_unique(values: list[Any], value: Any) -> None:
    if value not in values:
        values.append(value)


def artifact_ref(path: pathlib.Path, repo_root: pathlib.Path) -> dict[str, str]:
    return {
        "kind": "native_proof",
        "path": repo_relative(path, repo_root),
    }


def criteria_pass(proof: dict[str, Any], keys: tuple[str, ...]) -> bool:
    criteria = proof.get("criteria", {})
    return isinstance(criteria, dict) and all(criteria.get(key) is True for key in keys)


def attach_route_proof(report: dict[str, Any], proof_ref: str) -> None:
    for route in report.get("routes", []) or []:
        if not isinstance(route, dict) or route.get("chosen_route") not in NATIVE_ROUTES:
            continue
        refs = route.setdefault("validation_refs", [])
        if isinstance(refs, list):
            append_unique(refs, proof_ref)


def phase_g_matches(report: dict[str, Any], proof: dict[str, Any]) -> bool:
    return str(report.get("fixture_id", "")) == str(proof.get("fixture", ""))


def apply_phase_g_cpp_only(
    report: dict[str, Any],
    path: pathlib.Path,
    proof: dict[str, Any],
    repo_root: pathlib.Path,
) -> list[dict[str, Any]]:
    if proof.get("schema") != PHASE_G_CPP_ONLY_SCHEMA or not phase_g_matches(report, proof):
        return []

    proof_path = repo_relative(path, repo_root)
    validation = report.setdefault("validation", {})
    applied: list[dict[str, Any]] = []
    target = str(proof.get("target", ""))
    binary = proof.get("binary", {}) if isinstance(proof.get("binary"), dict) else {}
    cmake = proof.get("cmake", {}) if isinstance(proof.get("cmake"), dict) else {}

    compile_ok = proof.get("phase_g_cpp_only_proven") is True and criteria_pass(
        proof,
        (
            "target_built",
            "cpp_only_compile_flag_present",
            "target_links_view_core_only",
        ),
    )
    if compile_ok:
        compile_status: dict[str, Any] = {
            "status": "pass",
            "source": "phase_g_cpp_only",
            "proof_artifact": artifact_ref(path, repo_root),
        }
        if target:
            compile_status["target"] = target
        if isinstance(binary.get("path"), str) and binary["path"]:
            compile_status["binary_path"] = binary["path"]
        if isinstance(binary.get("sha256"), str) and binary["sha256"]:
            compile_status["binary_sha256"] = binary["sha256"]
        validation["compile"] = compile_status
        applied.append({
            "kind": "phase_g_cpp_only_compile",
            "path": proof_path,
            "status": "pass",
        })

    binary_ok = proof.get("phase_g_cpp_only_proven") is True and criteria_pass(
        proof,
        (
            "target_links_view_core_only",
            "binary_has_no_forbidden_script_symbols",
            "generated_source_has_no_script_symbol_needles",
            "js_engine_unavailable_in_target",
        ),
    )
    if binary_ok:
        dependency_status: dict[str, Any] = {
            "js_engine_present": False,
            "source": "phase_g_cpp_only",
            "proof_artifact": artifact_ref(path, repo_root),
        }
        if target:
            dependency_status["target"] = target
        if isinstance(binary.get("path"), str) and binary["path"]:
            dependency_status["binary_path"] = binary["path"]
        if isinstance(binary.get("sha256"), str) and binary["sha256"]:
            dependency_status["binary_sha256"] = binary["sha256"]
        if cmake.get("links_view_core_only") is True:
            dependency_status["target_links_view_core_only"] = True
        if cmake.get("cpp_only_flag_present") is True:
            dependency_status["cpp_only_flag_present"] = True
        validation["binary_dependencies"] = dependency_status
        applied.append({
            "kind": "phase_g_cpp_only_binary_dependencies",
            "path": proof_path,
            "status": "pass",
        })

    if applied:
        attach_route_proof(report, f"native_proof:{proof_path}")
    return applied


def row_matches_report(row: dict[str, Any], report: dict[str, Any]) -> bool:
    fixture_id = str(report.get("fixture_id", ""))
    source_path = report.get("source", {}).get("path")
    # Only match on fixture_id when the report actually carries one. Two
    # fixture-less artifacts both reporting "" must not be treated as the same
    # fixture, which would attach an unrelated compile row as this report's
    # proof.
    if fixture_id and row.get("fixture_id") == fixture_id:
        return True
    if isinstance(source_path, str) and source_path:
        return row.get("path") == source_path
    return False


def find_phase_h_compile_row(report: dict[str, Any], proof: dict[str, Any]) -> dict[str, Any] | None:
    rows = proof.get("rows", [])
    if not isinstance(rows, list):
        return None
    for row in rows:
        if isinstance(row, dict) and row_matches_report(row, report):
            return row
    return None


def apply_phase_h_compile_probe(
    report: dict[str, Any],
    path: pathlib.Path,
    proof: dict[str, Any],
    repo_root: pathlib.Path,
) -> list[dict[str, Any]]:
    if proof.get("schema") != PHASE_H_COMPILE_PROBE_SCHEMA:
        return []
    row = find_phase_h_compile_row(report, proof)
    if row is None or row.get("status") != "pass" or row.get("exit_code") != 0:
        return []

    proof_path = repo_relative(path, repo_root)
    compile_status: dict[str, Any] = {
        "status": "pass",
        "source": "phase_h_import_cpp_compile_probe",
        "proof_artifact": artifact_ref(path, repo_root),
    }
    for source_key, target_key in (
        ("fixture_id", "fixture_id"),
        ("source_cpp", "source_cpp"),
        ("source_header", "source_header"),
        ("object_path", "object_path"),
        ("exit_code", "exit_code"),
        ("object_bytes", "object_bytes"),
    ):
        value = row.get(source_key)
        if isinstance(value, str) and value:
            compile_status[target_key] = value
        elif is_non_negative_int(value):
            compile_status[target_key] = value

    report.setdefault("validation", {})["compile"] = compile_status
    attach_route_proof(report, f"native_proof:{proof_path}")
    return [{
        "kind": "phase_h_import_cpp_compile",
        "path": proof_path,
        "status": "pass",
    }]


def apply_native_proofs(
    report: dict[str, Any],
    proofs: list[tuple[pathlib.Path, dict[str, Any]]],
    repo_root: pathlib.Path,
) -> dict[str, Any]:
    applied: list[dict[str, Any]] = []
    for path, proof in proofs:
        applied.extend(apply_phase_g_cpp_only(report, path, proof, repo_root))
        applied.extend(apply_phase_h_compile_probe(report, path, proof, repo_root))

    if applied:
        validation = report.setdefault("validation", {})
        proof_entries = validation.setdefault("proofs", [])
        if isinstance(proof_entries, list):
            proof_entries.extend(applied)
        notes = validation.setdefault("notes", [])
        if isinstance(notes, list):
            for proof in applied:
                append_unique(notes, f"native proof applied: {proof['kind']} from {proof['path']}")
    return report
