#!/usr/bin/env python3
"""Tests for tools/scripts/frontend_ir_report.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parent / "frontend_ir_report.py"
spec = importlib.util.spec_from_file_location("frontend_ir_report", SCRIPT)
assert spec and spec.loader
fir = importlib.util.module_from_spec(spec)
sys.modules["frontend_ir_report"] = fir
spec.loader.exec_module(fir)


def write_json(path: pathlib.Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data), encoding="utf-8")


class FrontendIrReportTests(unittest.TestCase):
    def test_builds_report_from_route_manifest_and_source_audit(self) -> None:
        route_manifest = {
            "schema": "pulp-native-ui-route-manifest-v1",
            "fixture": "fixture-a",
            "inputs": {
                "sourceJsx": {
                    "path": "fixtures/UI.jsx",
                    "sha256": "a" * 64,
                },
                "ir": {
                    "path": "reports/generated/ui-ir.json",
                    "sha256": "b" * 64,
                },
            },
            "route_metrics": {
                "js_engine_initialized": True,
            },
            "source_contract_overlay": {
                "source": {
                    "source_of_truth": "archived_fixture",
                },
                "node_route_rows": [
                    {
                        "id": "knob.1",
                        "stable_source_path": "fixtures/UI.jsx:10:Knob[0]",
                        "source_line": 10,
                        "required_native_primitive": "knob",
                        "route_type": "native_cpp",
                        "style_token_references": ["colors.accent"],
                        "size": 48,
                        "label": "gain",
                        "confidence": 0.9,
                        "parameter_bindings": [
                            {
                                "param_key": "gain",
                                "binding_contract_id": "binding.gain",
                            }
                        ],
                        "gesture_contracts": [
                            {
                                "boundaries": ["begin", "update", "end"],
                            }
                        ],
                    }
                ],
            },
        }
        source_audit = {
            "lines": 20,
            "bytes": 1000,
            "sourceTemplateCounts": {
                "useState": 1,
                "useEffect": 0,
                "styleAttributes": 3,
            },
            "componentInvocationTemplates": {
                "Knob": 1,
            },
            "componentProps": {
                "Knob": ["value", "onChange"],
            },
        }

        report = fir.build_frontend_ir(
            route_manifest,
            source_audit,
            pathlib.Path("/repo/reports/route.json"),
            pathlib.Path("/repo"),
        )

        self.assertEqual(report["schema"], "pulp-frontend-ir-v0")
        self.assertEqual(report["fixture_id"], "fixture-a")
        self.assertEqual(report["source"]["kind"], "jsx")
        self.assertEqual(report["source"]["source_of_truth"], "archived_fixture")
        self.assertEqual(report["source"]["sha256"], "a" * 64)
        self.assertIn("react_state_hooks", report["source"]["dynamic_risks"])
        self.assertEqual(report["design_ir"]["path"], "reports/generated/ui-ir.json")
        self.assertEqual(report["design_ir"]["sha256"], "b" * 64)
        self.assertEqual(report["route_manifest"]["path"], "reports/route.json")
        self.assertEqual(report["nodes"][0]["id"], "knob.1")
        self.assertEqual(report["nodes"][0]["semantic_role"], "knob")
        self.assertEqual(report["nodes"][0]["source_span"]["line"], 10)
        self.assertEqual(report["nodes"][0]["state"]["parameters"][0]["id"], "gain")
        self.assertEqual(report["routes"][0]["chosen_route"], "native_cpp")
        self.assertFalse(report["routes"][0]["requires_js_engine"])
        self.assertIn("validation_refs", report["routes"][0])
        self.assertEqual(report["validation"]["source_counts"]["component_invocations"], 1)
        self.assertEqual(report["validation"]["style_counts"]["supported"], 2)
        self.assertTrue(report["validation"]["binary_dependencies"]["js_engine_present"])
        fir.validate_frontend_ir(report)

    def test_validator_rejects_native_route_without_validation_refs(self) -> None:
        report = {
            "schema": "pulp-frontend-ir-v0",
            "source": {
                "kind": "jsx",
                "path": "fixtures/UI.jsx",
                "source_of_truth": "local_file",
                "counts": {},
            },
            "design_ir": {
                "path": "reports/generated/ui-ir.json",
            },
            "nodes": [],
            "routes": [
                {
                    "node_id": "node-a",
                    "chosen_route": "native_cpp",
                    "reason": "missing proof",
                    "requires_js_engine": False,
                }
            ],
            "validation": {
                "source_counts": {},
                "style_counts": {},
            },
        }

        with self.assertRaisesRegex(ValueError, "validation_refs"):
            fir.validate_frontend_ir(report)

    def test_cli_writes_deterministic_json(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            route_manifest_path = root / "reports/route.json"
            source_audit_path = root / "reports/source.json"
            output_path = root / "reports/frontend-ir.json"
            write_json(
                route_manifest_path,
                {
                    "schema": "pulp-native-ui-route-manifest-v1",
                    "fixture": "fixture-b",
                    "inputs": {
                        "sourceJsx": {"path": "fixtures/UI.jsx"},
                        "ir": {"path": "reports/generated/ui-ir.json"},
                    },
                    "source_contract_overlay": {
                        "node_route_rows": [],
                    },
                },
            )
            write_json(
                source_audit_path,
                {
                    "sourceTemplateCounts": {},
                },
            )

            rc = fir.main([
                "--route-manifest",
                str(route_manifest_path),
                "--source-audit",
                str(source_audit_path),
                "--output",
                str(output_path),
                "--repo-root",
                str(root),
            ])

            self.assertEqual(rc, 0)
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(report["schema"], "pulp-frontend-ir-v0")
            self.assertEqual(report["route_manifest"]["path"], "reports/route.json")
            self.assertEqual(report["routes"], [])


if __name__ == "__main__":
    unittest.main()
