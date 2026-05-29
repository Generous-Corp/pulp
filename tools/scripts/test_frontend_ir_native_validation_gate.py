#!/usr/bin/env python3
"""Tests for tools/scripts/frontend_ir_native_validation_gate.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parent / "frontend_ir_native_validation_gate.py"
spec = importlib.util.spec_from_file_location("frontend_ir_native_validation_gate", SCRIPT)
assert spec and spec.loader
native_gate = importlib.util.module_from_spec(spec)
sys.modules["frontend_ir_native_validation_gate"] = native_gate
spec.loader.exec_module(native_gate)


def ready_gate(schema: str, fixture_id: str = "panel", warnings: int = 0) -> dict:
    checks = [{"id": "schema_valid", "status": "pass", "message": "ok"}]
    if warnings:
        checks.append({"id": "review_me", "status": "warn", "message": "review"})
    return {
        "schema": schema,
        "fixture_id": fixture_id,
        "verdict": "ready",
        "summary": {"checks": len(checks), "failures": 0, "warnings": warnings},
        "checks": checks,
    }


def cpp_only_report() -> dict:
    return {
        "schema": "pulp-native-ui-phase-g-cpp-only-audit-v1",
        "fixture": "panel",
        "phase_g_cpp_only_proven": True,
        "binary": {
            "exists": True,
            "bytes": 1234,
            "sha256": "a" * 64,
        },
        "criteria": {
            "eligible_fixture_selected": True,
            "target_built": True,
            "cpp_only_compile_flag_present": True,
            "target_links_view_core_only": True,
            "binary_has_no_forbidden_script_symbols": True,
            "generated_source_has_no_script_symbol_needles": True,
            "visual_and_behavior_parity_pass": True,
            "all_contracts_bound": True,
            "js_engine_unavailable_in_target": True,
        },
    }


def behavior_visual_report() -> dict:
    return {
        "schema": "pulp-native-ui-phase-g-cpp-only-behavior-visual-v1",
        "compiled_with_cpp_only_flag": True,
        "target_links_script_runtime": False,
        "threshold": 0.9,
        "within_threshold": True,
        "full_within_threshold": True,
        "full_similarity": 0.98,
        "routed_region_similarity": 0.95,
        "bound_counts": {"knobs": 1, "faders": 1},
        "behavior": {
            "passed": True,
            "interaction_count": 4,
            "parameter_event_count": 3,
        },
    }


def cost_report() -> dict:
    return {
        "schema": "pulp-native-ui-phase-g-cpp-only-cost-audit-v1",
        "status": "complete",
        "criteria": {"complete": True},
        "binary_size_delta": {
            "cpp_only_bytes": 1234,
            "live_runtime_reference_bytes": 5678,
        },
        "cost_metrics": {
            "startup": {"first_frame_ms": 1.25},
        },
    }


def gpu_preview_report() -> dict:
    return {
        "schema": "pulp-native-ui-phase-f-gpu-preview-audit-v1",
        "gpu_preview_proven": True,
        "gpu_linked": True,
        "no_js_engine_symbols": True,
        "runtime": {
            "loaded_gpu_runtime": True,
            "loaded_js_runtime": False,
            "hits": ["Metal"],
        },
    }


def build_gate(**overrides: dict) -> dict:
    args = {
        "frontend_ir_gate": ready_gate("pulp-frontend-ir-gate-v0"),
        "primitive_gate": ready_gate("pulp-frontend-ir-primitive-gate-v0", warnings=1),
        "codegen_artifact_gate": ready_gate("pulp-frontend-ir-codegen-artifact-gate-v0", warnings=2),
        "cpp_only_report": cpp_only_report(),
        "behavior_visual_report": behavior_visual_report(),
        "cost_report": cost_report(),
        "gpu_preview_report": gpu_preview_report(),
    }
    args.update(overrides)
    return native_gate.build_native_validation_gate(**args)


class FrontendIrNativeValidationGateTests(unittest.TestCase):
    def test_ready_gate_carries_child_warnings(self) -> None:
        gate = build_gate()

        self.assertEqual(gate["schema"], "pulp-frontend-ir-native-validation-gate-v0")
        self.assertEqual(gate["verdict"], "ready")
        self.assertEqual(gate["summary"]["failures"], 0)
        warning_ids = {item["id"] for item in gate["checks"] if item["status"] == "warn"}
        self.assertIn("primitive_native_readiness_gate_warnings", warning_ids)
        self.assertIn("codegen_artifact_gate_warnings", warning_ids)

    def test_not_ready_child_gate_fails(self) -> None:
        child = ready_gate("pulp-frontend-ir-gate-v0")
        child["verdict"] = "not_ready"
        child["summary"]["failures"] = 1

        gate = build_gate(frontend_ir_gate=child)

        self.assertEqual(gate["verdict"], "not_ready")
        failures = {item["id"] for item in gate["checks"] if item["status"] == "fail"}
        self.assertIn("frontend_ir_native_readiness_gate", failures)

    def test_behavior_without_interactions_fails(self) -> None:
        behavior = behavior_visual_report()
        behavior["behavior"]["interaction_count"] = 0

        gate = build_gate(behavior_visual_report=behavior)

        self.assertEqual(gate["verdict"], "not_ready")
        failures = {item["id"] for item in gate["checks"] if item["status"] == "fail"}
        self.assertIn("behavior_visual_parity", failures)

    def test_missing_optional_reports_warn_without_failing(self) -> None:
        gate = build_gate(cost_report=None, gpu_preview_report=None)

        self.assertEqual(gate["verdict"], "ready")
        warning_ids = {item["id"] for item in gate["checks"] if item["status"] == "warn"}
        self.assertIn("cost_proof", warning_ids)
        self.assertIn("gpu_preview_proof", warning_ids)

    def test_cli_writes_gate_and_respects_allow_not_ready(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            files = {
                "frontend": ready_gate("pulp-frontend-ir-gate-v0"),
                "primitive": ready_gate("pulp-frontend-ir-primitive-gate-v0"),
                "codegen": ready_gate("pulp-frontend-ir-codegen-artifact-gate-v0"),
                "cpp": cpp_only_report(),
                "behavior": behavior_visual_report(),
            }
            files["cpp"]["criteria"]["target_built"] = False
            paths = {}
            for name, payload in files.items():
                path = root / f"{name}.json"
                path.write_text(json.dumps(payload), encoding="utf-8")
                paths[name] = path
            output = root / "gate.json"

            rc = native_gate.main([
                "--frontend-ir-gate", str(paths["frontend"]),
                "--primitive-gate", str(paths["primitive"]),
                "--codegen-artifact-gate", str(paths["codegen"]),
                "--cpp-only-report", str(paths["cpp"]),
                "--behavior-visual-report", str(paths["behavior"]),
                "--output", str(output),
                "--allow-not-ready",
            ])

            self.assertEqual(rc, 0)
            written = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(written["verdict"], "not_ready")


if __name__ == "__main__":
    unittest.main()
