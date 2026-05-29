#!/usr/bin/env python3
"""Tests for tools/scripts/frontend_ir_codegen_artifact_gate.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parent / "frontend_ir_codegen_artifact_gate.py"
spec = importlib.util.spec_from_file_location("frontend_ir_codegen_artifact_gate", SCRIPT)
assert spec and spec.loader
codegen_gate = importlib.util.module_from_spec(spec)
sys.modules["frontend_ir_codegen_artifact_gate"] = codegen_gate
spec.loader.exec_module(codegen_gate)


def artifact_ref(kind: str = "artifact") -> dict:
    return {
        "kind": kind,
        "path": f"{kind}.json",
        "sha256": "a" * 64,
        "byte_size": 12,
    }


def sample_report() -> dict:
    return {
        "schema": "pulp-frontend-ir-codegen-artifacts-v0",
        "fixture_id": "panel",
        "frontend_ir": artifact_ref("frontend_ir"),
        "artifacts": {
            "source_cpp": artifact_ref("generated_cpp_source"),
            "header": artifact_ref("generated_cpp_header"),
            "binding_manifest": artifact_ref("generated_cpp_binding_manifest"),
        },
        "summary": {
            "native_cpp_routes": 2,
            "binding_entries": 3,
            "directly_bound_native_routes": 1,
            "missing_native_route_bindings": 1,
            "extra_binding_entries": 2,
            "split_binding_candidates": 1,
            "binding_primitives": {"knob": 1, "meter": 2},
            "binding_route_types": {"native_cpp": 3},
        },
        "missing_native_route_bindings": ["panel.meter.out"],
        "extra_binding_entries": ["panel.meter.out.left", "panel.meter.out.right"],
        "split_binding_candidates": [
            {
                "route_id": "panel.meter.out",
                "binding_entry_ids": ["panel.meter.out.left", "panel.meter.out.right"],
            }
        ],
    }


class FrontendIrCodegenArtifactGateTests(unittest.TestCase):
    def test_split_missing_route_warns_but_is_ready(self) -> None:
        gate = codegen_gate.gate_codegen_artifacts(sample_report())

        self.assertEqual(gate["schema"], "pulp-frontend-ir-codegen-artifact-gate-v0")
        self.assertEqual(gate["verdict"], "ready")
        self.assertEqual(gate["summary"]["failures"], 0)
        warnings = {item["id"] for item in gate["checks"] if item["status"] == "warn"}
        self.assertIn("direct_binding_coverage", warnings)
        self.assertIn("extra_binding_entries", warnings)

    def test_unaccounted_missing_route_fails(self) -> None:
        report = sample_report()
        report["missing_native_route_bindings"].append("panel.knob.extra")
        report["summary"]["native_cpp_routes"] = 3
        report["summary"]["missing_native_route_bindings"] = 2

        gate = codegen_gate.gate_codegen_artifacts(report)

        self.assertEqual(gate["verdict"], "not_ready")
        failures = {item["id"] for item in gate["checks"] if item["status"] == "fail"}
        self.assertIn("unaccounted_native_route_bindings", failures)

    def test_inconsistent_summary_counts_fail_schema_validation(self) -> None:
        report = sample_report()
        report["summary"]["missing_native_route_bindings"] = 0

        gate = codegen_gate.gate_codegen_artifacts(report)

        self.assertEqual(gate["verdict"], "not_ready")
        failures = [item for item in gate["checks"] if item["status"] == "fail"]
        self.assertEqual(len(failures), 1)
        self.assertEqual(failures[0]["id"], "schema_valid")
        self.assertIn("must match", failures[0]["message"])

    def test_non_native_binding_route_types_warn(self) -> None:
        report = sample_report()
        report["summary"]["binding_route_types"] = {
            "native_cpp": 2,
            "native_custom_paint": 1,
        }

        gate = codegen_gate.gate_codegen_artifacts(report)

        warnings = {item["id"] for item in gate["checks"] if item["status"] == "warn"}
        self.assertIn("non_native_cpp_binding_route_types", warnings)

    def test_cli_writes_gate_and_respects_allow_not_ready(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            report_path = root / "codegen-artifacts.json"
            output_path = root / "codegen-artifact-gate.json"
            report = sample_report()
            report["missing_native_route_bindings"].append("panel.unaccounted")
            report["summary"]["native_cpp_routes"] = 3
            report["summary"]["missing_native_route_bindings"] = 2
            report_path.write_text(json.dumps(report), encoding="utf-8")

            rc = codegen_gate.main([
                "--codegen-artifacts",
                str(report_path),
                "--output",
                str(output_path),
                "--allow-not-ready",
            ])

            self.assertEqual(rc, 0)
            written = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(written["verdict"], "not_ready")
            self.assertEqual(written["summary"]["failures"], 1)

    def test_cli_returns_failure_for_not_ready_without_override(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            report_path = root / "codegen-artifacts.json"
            output_path = root / "codegen-artifact-gate.json"
            report = sample_report()
            report["summary"]["binding_entries"] = 0
            report_path.write_text(json.dumps(report), encoding="utf-8")

            rc = codegen_gate.main([
                "--codegen-artifacts",
                str(report_path),
                "--output",
                str(output_path),
            ])

            self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
