#!/usr/bin/env python3
"""Evaluate native end-to-end readiness from existing FrontendIR proof artifacts.

This gate is an *evidence aggregator* over child verdicts: it does not re-run any
upstream validation. It trusts each child artifact's own pass/fail determination
and re-derives only the cross-cutting invariants needed to keep a child from
contributing a silent or self-certified PASS to the aggregate verdict. The
per-function docstrings below spell out the trust boundary each check enforces.
"""

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
GATE_SCHEMA = SCHEMAS["native_validation_gate"]

FRONTEND_IR_GATE_SCHEMA = SCHEMAS["gate"]
PRIMITIVE_GATE_SCHEMA = SCHEMAS["primitive_gate"]
CODEGEN_GATE_SCHEMA = SCHEMAS["codegen_artifact_gate"]
CPP_ONLY_SCHEMA = "pulp-native-ui-phase-g-cpp-only-audit-v1"
BEHAVIOR_VISUAL_SCHEMA = "pulp-native-ui-phase-g-cpp-only-behavior-visual-v1"
COST_SCHEMA = "pulp-native-ui-phase-g-cpp-only-cost-audit-v1"
GPU_PREVIEW_SCHEMA = "pulp-native-ui-phase-f-gpu-preview-audit-v1"

CPP_ONLY_CRITERIA = (
    "eligible_fixture_selected",
    "target_built",
    "cpp_only_compile_flag_present",
    "target_links_view_core_only",
    "binary_has_no_forbidden_script_symbols",
    "generated_source_has_no_script_symbol_needles",
    "visual_and_behavior_parity_pass",
    "all_contracts_bound",
    "js_engine_unavailable_in_target",
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


def child_gate_check(report: dict[str, Any],
                     *,
                     expected_schema: str,
                     check_id: str,
                     label: str) -> list[dict[str, Any]]:
    """Aggregate a child gate's verdict (FrontendIR / primitive / codegen).

    TRUST BOUNDARY: trusts the child gate's own ``verdict`` and ``summary``
    counts as the authoritative result of the validation it ran — this gate does
    not re-run that validation. It independently re-derives only the structural
    guards that prevent a vacuous PASS: the report must carry the expected
    ``schema``, must contain at least one ``checks`` entry (a "ready" verdict
    with no checks has verified nothing), and must report ``verdict == ready``
    with zero failures. Child warnings are surfaced but not treated as failing.
    """
    checks: list[dict[str, Any]] = []
    if report.get("schema") != expected_schema:
        return [check(check_id, FAIL_STATUS, f"{label} schema must be {expected_schema}")]
    verdict = report.get("verdict")
    summary = as_dict(report.get("summary"))
    warnings = non_negative_int(summary.get("warnings"))
    failures = non_negative_int(summary.get("failures"))
    child_checks = as_list(report.get("checks"))
    if not child_checks:
        # A child gate that reports "ready" with no checks has verified nothing;
        # it must not contribute a silent PASS to the aggregate gate.
        return [check(
            check_id,
            FAIL_STATUS,
            f"{label} reported no checks to verify",
            verdict=verdict,
        )]
    warning_ids = [
        str(item.get("id", ""))
        for item in child_checks
        if isinstance(item, dict) and item.get("status") == WARN_STATUS and item.get("id")
    ]

    if verdict != READY_VERDICT or failures > 0:
        return [check(
            check_id,
            FAIL_STATUS,
            f"{label} is not ready",
            verdict=verdict,
            failures=failures,
        )]

    checks.append(check(
        check_id,
        PASS_STATUS,
        f"{label} is ready",
        warnings=warnings,
    ))
    if warnings > 0:
        checks.append(check(
            f"{check_id}_warnings",
            WARN_STATUS,
            f"{label} has reviewable warnings",
            warnings=warnings,
            warning_ids=warning_ids,
        ))
    return checks


def cpp_only_checks(report: dict[str, Any]) -> list[dict[str, Any]]:
    """Confirm the phase-G cpp-only audit proves a no-runtime-JS native binary.

    TRUST BOUNDARY: trusts the audit's reported ``criteria`` booleans, its
    ``binary`` descriptor (existence, byte count, sha256), and its
    ``phase_g_cpp_only_proven`` claim as produced facts — it does not re-run the
    build or re-hash the binary. It independently re-derives the *completeness*
    conjunction: the schema must match, every required criterion must be exactly
    ``True``, the binary must exist with non-zero bytes and a 64-char sha256, and
    the proven flag must be set. A missing field reads as not-proven, never PASS.
    """
    if report.get("schema") != CPP_ONLY_SCHEMA:
        return [check("cpp_only_audit", FAIL_STATUS, f"cpp-only audit schema must be {CPP_ONLY_SCHEMA}")]

    criteria = as_dict(report.get("criteria"))
    missing = [name for name in CPP_ONLY_CRITERIA if criteria.get(name) is not True]
    binary = as_dict(report.get("binary"))
    binary_ok = (
        binary.get("exists") is True and
        non_negative_int(binary.get("bytes")) > 0 and
        isinstance(binary.get("sha256"), str) and
        len(binary["sha256"]) == 64
    )
    proven = report.get("phase_g_cpp_only_proven") is True

    if not proven or missing or not binary_ok:
        return [check(
            "cpp_only_audit",
            FAIL_STATUS,
            "cpp-only audit is not fully proven",
            phase_g_cpp_only_proven=proven,
            missing_criteria=missing,
            binary_ok=binary_ok,
        )]

    return [check(
        "cpp_only_audit",
        PASS_STATUS,
        "cpp-only audit proves no-runtime-JS native output",
        binary_bytes=binary["bytes"],
    )]


def behavior_visual_checks(report: dict[str, Any]) -> list[dict[str, Any]]:
    """Confirm the phase-G behavior/visual parity proof is complete and passing.

    TRUST BOUNDARY: trusts the report's measured numbers (``threshold``,
    ``full_similarity``, ``routed_region_similarity``, interaction/parameter
    counts, ``bound_counts``) and its boolean claims as produced evidence — it
    does not re-capture screenshots or replay interactions. It independently
    re-derives the guards that stop a report from self-certifying: the schema
    must match, it must compile cpp-only and not link the script runtime, a
    *positive* similarity threshold must be present (a zero threshold makes the
    ``<`` comparison toothless), measured similarity must clear that threshold,
    behavior must have passed, and interaction / parameter / bound counts must
    all be non-zero so the proof actually exercised the UI.
    """
    if report.get("schema") != BEHAVIOR_VISUAL_SCHEMA:
        return [check(
            "behavior_visual_parity",
            FAIL_STATUS,
            f"behavior/visual report schema must be {BEHAVIOR_VISUAL_SCHEMA}",
        )]

    threshold = float(report.get("threshold", 0.0) or 0.0)
    full_similarity = float(report.get("full_similarity", 0.0) or 0.0)
    routed_similarity = float(report.get("routed_region_similarity", 0.0) or 0.0)
    behavior = as_dict(report.get("behavior"))
    interaction_count = non_negative_int(behavior.get("interaction_count"))
    parameter_event_count = non_negative_int(behavior.get("parameter_event_count"))
    bound_counts = as_dict(report.get("bound_counts"))
    bound_total = sum(non_negative_int(value) for value in bound_counts.values())
    failures: list[str] = []

    if report.get("compiled_with_cpp_only_flag") is not True:
        failures.append("compiled_with_cpp_only_flag")
    if report.get("target_links_script_runtime") is not False:
        failures.append("target_links_script_runtime")
    if report.get("within_threshold") is not True or report.get("full_within_threshold") is not True:
        failures.append("within_threshold")
    if threshold <= 0.0:
        # Without a positive threshold the similarity comparison below has no
        # teeth (`x < 0.0` is never true), so a report could self-certify
        # parity with zero measured similarity. Require a real threshold.
        failures.append("missing_similarity_threshold")
    if full_similarity < threshold or routed_similarity < threshold:
        failures.append("similarity_threshold")
    if behavior.get("passed") is not True:
        failures.append("behavior_passed")
    if interaction_count == 0 or parameter_event_count == 0:
        failures.append("interaction_trace")
    if bound_total == 0:
        failures.append("bound_counts")

    if failures:
        return [check(
            "behavior_visual_parity",
            FAIL_STATUS,
            "behavior/visual parity proof is incomplete",
            failures=failures,
            threshold=threshold,
            full_similarity=full_similarity,
            routed_region_similarity=routed_similarity,
        )]

    return [check(
        "behavior_visual_parity",
        PASS_STATUS,
        "behavior/visual parity proof passed",
        threshold=threshold,
        full_similarity=full_similarity,
        routed_region_similarity=routed_similarity,
        interactions=interaction_count,
        parameter_events=parameter_event_count,
    )]


def cost_checks(report: dict[str, Any] | None) -> list[dict[str, Any]]:
    """Confirm the optional phase-G cost proof is complete when supplied.

    TRUST BOUNDARY: the cost proof is optional — a missing report is a WARN, not
    a failure, so its absence cannot block readiness. When present, this trusts
    the report's ``status``, ``binary_size_delta`` and ``cost_metrics`` as
    produced measurements (no re-measurement here) and independently re-derives
    completeness: the schema must match, status and ``criteria.complete`` must
    both signal complete, and the cpp-only byte delta plus startup metrics must
    be present and non-empty.
    """
    if report is None:
        return [check("cost_proof", WARN_STATUS, "cost proof was not supplied")]
    if report.get("schema") != COST_SCHEMA:
        return [check("cost_proof", FAIL_STATUS, f"cost report schema must be {COST_SCHEMA}")]
    criteria = as_dict(report.get("criteria"))
    if report.get("status") != "complete" or criteria.get("complete") is not True:
        return [check("cost_proof", FAIL_STATUS, "cost proof is not complete")]
    delta = as_dict(report.get("binary_size_delta"))
    startup = as_dict(as_dict(report.get("cost_metrics")).get("startup"))
    if non_negative_int(delta.get("cpp_only_bytes")) == 0 or not startup:
        return [check("cost_proof", FAIL_STATUS, "cost proof is missing binary delta or startup metrics")]
    return [check(
        "cost_proof",
        PASS_STATUS,
        "cost proof is complete",
        cpp_only_bytes=delta.get("cpp_only_bytes"),
        live_runtime_reference_bytes=delta.get("live_runtime_reference_bytes"),
        first_frame_ms=startup.get("first_frame_ms"),
    )]


def gpu_preview_checks(report: dict[str, Any] | None) -> list[dict[str, Any]]:
    """Confirm the optional phase-F GPU preview proof loaded GPU but not JS.

    TRUST BOUNDARY: the GPU preview proof is optional — a missing report is a
    WARN, not a failure. When present, this trusts the report's boolean claims
    and ``runtime`` observations as produced facts and independently re-derives
    completeness: the schema must match, ``gpu_preview_proven`` / ``gpu_linked``
    / ``no_js_engine_symbols`` must each be ``True``, the GPU runtime must have
    loaded, and the JS runtime must explicitly *not* have loaded.
    """
    if report is None:
        return [check("gpu_preview_proof", WARN_STATUS, "GPU preview proof was not supplied")]
    if report.get("schema") != GPU_PREVIEW_SCHEMA:
        return [check("gpu_preview_proof", FAIL_STATUS, f"GPU preview report schema must be {GPU_PREVIEW_SCHEMA}")]
    runtime = as_dict(report.get("runtime"))
    failures: list[str] = []
    for key in ("gpu_preview_proven", "gpu_linked", "no_js_engine_symbols"):
        if report.get(key) is not True:
            failures.append(key)
    if runtime.get("loaded_gpu_runtime") is not True:
        failures.append("loaded_gpu_runtime")
    if runtime.get("loaded_js_runtime") is not False:
        failures.append("loaded_js_runtime")
    if failures:
        return [check("gpu_preview_proof", FAIL_STATUS, "GPU preview proof is incomplete", failures=failures)]
    return [check(
        "gpu_preview_proof",
        PASS_STATUS,
        "GPU preview proof passed with no JS runtime loaded",
        runtime_hits=runtime.get("hits", []),
    )]


def infer_fixture_id(*reports: dict[str, Any]) -> str:
    for report in reports:
        value = report.get("fixture_id", report.get("fixture", ""))
        if isinstance(value, str) and value:
            return value
    return ""


def build_native_validation_gate(
    *,
    frontend_ir_gate: dict[str, Any],
    primitive_gate: dict[str, Any],
    codegen_artifact_gate: dict[str, Any],
    cpp_only_report: dict[str, Any],
    behavior_visual_report: dict[str, Any],
    cost_report: dict[str, Any] | None = None,
    gpu_preview_report: dict[str, Any] | None = None,
    artifact_paths: dict[str, str] | None = None,
) -> dict[str, Any]:
    """Compose all child verdicts into the aggregate native-validation gate.

    TRUST BOUNDARY: this is the top-level aggregator. It owns no validation of
    its own — it trusts each child check function (above) to enforce its
    respective trust boundary and simply unions their emitted checks. The
    aggregate ``verdict`` is mechanically derived: ``ready`` iff zero FAIL checks
    across all children (WARN does not block). The required FrontendIR /
    primitive / codegen child gates are mandatory inputs; the cost and GPU
    preview proofs are optional and contribute WARN-only when absent.
    """
    checks: list[dict[str, Any]] = []
    checks.extend(child_gate_check(
        frontend_ir_gate,
        expected_schema=FRONTEND_IR_GATE_SCHEMA,
        check_id="frontend_ir_native_readiness_gate",
        label="FrontendIR native-readiness gate",
    ))
    checks.extend(child_gate_check(
        primitive_gate,
        expected_schema=PRIMITIVE_GATE_SCHEMA,
        check_id="primitive_native_readiness_gate",
        label="primitive native-readiness gate",
    ))
    checks.extend(child_gate_check(
        codegen_artifact_gate,
        expected_schema=CODEGEN_GATE_SCHEMA,
        check_id="codegen_artifact_gate",
        label="codegen artifact gate",
    ))
    checks.extend(cpp_only_checks(cpp_only_report))
    checks.extend(behavior_visual_checks(behavior_visual_report))
    checks.extend(cost_checks(cost_report))
    checks.extend(gpu_preview_checks(gpu_preview_report))

    failures = sum(1 for item in checks if item["status"] == FAIL_STATUS)
    warnings = sum(1 for item in checks if item["status"] == WARN_STATUS)
    result = {
        "schema": GATE_SCHEMA,
        "fixture_id": infer_fixture_id(frontend_ir_gate, primitive_gate, codegen_artifact_gate, cpp_only_report),
        "verdict": READY_VERDICT if failures == 0 else NOT_READY_VERDICT,
        "summary": {
            "checks": len(checks),
            "failures": failures,
            "warnings": warnings,
        },
        "checks": checks,
    }
    if artifact_paths:
        result["artifacts"] = artifact_paths
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frontend-ir-gate", required=True, type=pathlib.Path)
    parser.add_argument("--primitive-gate", required=True, type=pathlib.Path)
    parser.add_argument("--codegen-artifact-gate", required=True, type=pathlib.Path)
    parser.add_argument("--cpp-only-report", required=True, type=pathlib.Path)
    parser.add_argument("--behavior-visual-report", required=True, type=pathlib.Path)
    parser.add_argument("--cost-report", type=pathlib.Path)
    parser.add_argument("--gpu-preview-report", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--allow-not-ready", action="store_true",
                        help="write the gate report but return success even when verdict is not_ready")
    args = parser.parse_args(argv)

    paths = {
        "frontend_ir_gate": args.frontend_ir_gate.as_posix(),
        "primitive_gate": args.primitive_gate.as_posix(),
        "codegen_artifact_gate": args.codegen_artifact_gate.as_posix(),
        "cpp_only_report": args.cpp_only_report.as_posix(),
        "behavior_visual_report": args.behavior_visual_report.as_posix(),
    }
    if args.cost_report:
        paths["cost_report"] = args.cost_report.as_posix()
    if args.gpu_preview_report:
        paths["gpu_preview_report"] = args.gpu_preview_report.as_posix()

    gate = build_native_validation_gate(
        frontend_ir_gate=load_json(args.frontend_ir_gate),
        primitive_gate=load_json(args.primitive_gate),
        codegen_artifact_gate=load_json(args.codegen_artifact_gate),
        cpp_only_report=load_json(args.cpp_only_report),
        behavior_visual_report=load_json(args.behavior_visual_report),
        cost_report=load_json(args.cost_report) if args.cost_report else None,
        gpu_preview_report=load_json(args.gpu_preview_report) if args.gpu_preview_report else None,
        artifact_paths=paths,
    )
    if args.output:
        write_json(args.output, gate)
    else:
        print(json.dumps(gate, indent=2, sort_keys=True))
    if gate["verdict"] != READY_VERDICT and not args.allow_not_ready:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
