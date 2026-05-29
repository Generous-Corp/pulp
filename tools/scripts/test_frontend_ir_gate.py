#!/usr/bin/env python3
"""Tests for tools/scripts/frontend_ir_gate.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
SCRIPT = SCRIPT_DIR / "frontend_ir_gate.py"
spec = importlib.util.spec_from_file_location("frontend_ir_gate", SCRIPT)
assert spec and spec.loader
gate = importlib.util.module_from_spec(spec)
sys.modules["frontend_ir_gate"] = gate
spec.loader.exec_module(gate)


def ready_report() -> dict:
    return {
        "schema": "pulp-frontend-ir-v0",
        "fixture_id": "fixture-a",
        "source": {
            "kind": "jsx",
            "path": "fixtures/UI.jsx",
            "source_of_truth": "local_file",
            "counts": {
                "source_contract_rows": 1,
                "source_contract_rows_with_source_span": 1,
            },
        },
        "design_ir": {
            "path": "reports/generated/ui-ir.json",
        },
        "nodes": [
            {
                "id": "node-a",
                "semantic_role": "knob",
                "source_span": {
                    "node_id": "node-a",
                    "path": "fixtures/UI.jsx:10:Knob[0]",
                    "line": 10,
                },
                "style": {
                    "layout": {},
                    "paint_layers": [],
                    "typography": {},
                    "variants": {},
                },
                "state": {
                    "parameters": [],
                    "meters": [],
                    "local_ui": {},
                    "derived": {},
                    "dynamic_risk": [],
                },
            }
        ],
        "resources": [
            {
                "id": "input.source_jsx",
                "original_uri": "fixtures/UI.jsx",
                "route_usage": ["native_cpp"],
                "sha256": "a" * 64,
                "byte_size": 24,
                "mime": "text/jsx",
            }
        ],
        "routes": [
            {
                "node_id": "node-a",
                "semantic_role": "knob",
                "chosen_route": "native_cpp",
                "reason": "source contract maps to knob",
                "requires_js_engine": False,
                "requires_gpu": True,
                "validation_refs": ["route_manifest:node-a"],
            }
        ],
        "validation": {
            "source_counts": {
                "source_contract_rows": 1,
                "source_contract_rows_with_source_span": 1,
            },
            "style_counts": {
                "supported": 1,
                "unsupported": 0,
                "support_native_cpp_present": 1,
            },
            "resource_counts": {
                "total": 1,
                "with_sha256": 1,
                "with_byte_size": 1,
                "watchable": 1,
            },
            "token_counts": {
                "total": 0,
                "unresolved": 0,
                "referenced_by_rows": 0,
            },
            "compile": {
                "status": "pass",
            },
            "binary_dependencies": {
                "js_engine_present": False,
                "proof_artifact": {
                    "kind": "native_proof",
                    "path": "reports/native-proof.json",
                },
            },
        },
    }


class FrontendIrGateTests(unittest.TestCase):
    def test_evidence_gate_passes_complete_report(self) -> None:
        result = gate.gate_frontend_ir(ready_report(), "evidence")

        self.assertEqual(result["verdict"], "ready")
        self.assertEqual(result["summary"]["failures"], 0)
        self.assertIn("resource_byte_hash_coverage", {item["id"] for item in result["checks"]})

    def test_evidence_gate_fails_missing_resource_hash(self) -> None:
        report = ready_report()
        report["resources"][0].pop("sha256")
        report["validation"]["resource_counts"]["with_sha256"] = 0

        result = gate.gate_frontend_ir(report, "evidence")

        self.assertEqual(result["verdict"], "not_ready")
        failures = {item["id"] for item in result["checks"] if item["status"] == "fail"}
        self.assertIn("resource_byte_hash_coverage", failures)

    def test_evidence_gate_warns_without_style_evidence(self) -> None:
        report = ready_report()
        report["validation"]["style_counts"] = {
            "supported": 0,
            "unsupported": 0,
        }

        result = gate.gate_frontend_ir(report, "evidence")

        self.assertEqual(result["verdict"], "ready")
        warnings = {item["id"] for item in result["checks"] if item["status"] == "warn"}
        self.assertIn("style_evidence_present", warnings)

    def test_native_readiness_fails_live_js_compile_and_token_gaps(self) -> None:
        report = ready_report()
        report["routes"][0]["chosen_route"] = "live_js"
        report["routes"][0]["requires_js_engine"] = True
        report["routes"][0]["fallback_reason"] = "requires runtime behavior"
        report["validation"]["binary_dependencies"]["js_engine_present"] = True
        report["validation"]["compile"]["status"] = "not_run"
        report["validation"]["token_counts"]["total"] = 1
        report["validation"]["token_counts"]["unresolved"] = 1

        result = gate.gate_frontend_ir(report, "native-readiness")

        self.assertEqual(result["verdict"], "not_ready")
        failures = {item["id"] for item in result["checks"] if item["status"] == "fail"}
        self.assertIn("native_routes_no_js", failures)
        self.assertIn("binary_no_js_engine", failures)
        self.assertIn("native_compile_pass", failures)
        self.assertIn("token_resolution", failures)

    def test_native_readiness_requires_proof_backed_no_js_evidence(self) -> None:
        report = ready_report()
        report["validation"]["binary_dependencies"].pop("proof_artifact")

        result = gate.gate_frontend_ir(report, "native-readiness")

        self.assertEqual(result["verdict"], "not_ready")
        failures = {item["id"] for item in result["checks"] if item["status"] == "fail"}
        self.assertIn("binary_no_js_engine", failures)

    def test_native_readiness_fails_route_invalidating_tweak(self) -> None:
        report = ready_report()
        report["tweaks"] = [
            {
                "node_id": "node-a",
                "property": "route.chosen_route",
                "value": "live_js",
                "invalidates": ["route"],
                "classification_preserved": False,
            }
        ]
        report["validation"]["tweak_counts"] = {
            "total": 1,
            "classification_preserved": 0,
            "invalidates_route": 1,
        }

        result = gate.gate_frontend_ir(report, "native-readiness")

        self.assertEqual(result["verdict"], "not_ready")
        failures = {item["id"] for item in result["checks"] if item["status"] == "fail"}
        self.assertIn("tweaks_preserve_classification", failures)

    def test_cli_writes_report_and_can_allow_not_ready(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            frontend_ir = root / "frontend-ir.json"
            output = root / "gate.json"
            report = ready_report()
            report["validation"]["compile"]["status"] = "not_run"
            frontend_ir.write_text(json.dumps(report), encoding="utf-8")

            rc = gate.main([
                "--frontend-ir",
                str(frontend_ir),
                "--mode",
                "native-readiness",
                "--output",
                str(output),
            ])
            self.assertEqual(rc, 1)

            rc = gate.main([
                "--frontend-ir",
                str(frontend_ir),
                "--mode",
                "native-readiness",
                "--output",
                str(output),
                "--allow-not-ready",
            ])
            self.assertEqual(rc, 0)
            result = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(result["verdict"], "not_ready")


if __name__ == "__main__":
    unittest.main()
