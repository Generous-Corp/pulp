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
                "nodes_total": 9,
                "interactive_controls_total": 1,
                "native_cpp_candidate_node_routes": 1,
                "js_engine_initialized": True,
                "requires_runtime_js": True,
                "behavior_pass": False,
            },
            "component_family_coverage": {
                "source_component_names_total": 1,
                "source_component_names_classified": 1,
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
                        "recorder_eligibility": "candidate",
                        "parameter_bindings": [
                            {
                                "param_key": "gain",
                                "binding_contract_id": "binding.gain",
                            }
                        ],
                        "event_contracts": [
                            {
                                "kind": "set_param",
                                "param_key": "gain",
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
            "componentInvocationProps": [
                {
                    "name": "Knob",
                    "line": 10,
                    "props": ["value", "onChange"],
                }
            ],
            "componentProps": {
                "Knob": ["value", "onChange"],
            },
            "styleKeys": ["width", "background"],
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
        self.assertEqual(report["source"]["spans"][0]["node_id"], "knob.1")
        self.assertEqual(report["nodes"][0]["state"]["parameters"][0]["id"], "gain")
        self.assertEqual(report["routes"][0]["chosen_route"], "native_cpp")
        self.assertFalse(report["routes"][0]["requires_js_engine"])
        self.assertIn("validation_refs", report["routes"][0])
        self.assertEqual(report["validation"]["source_counts"]["component_invocations"], 1)
        self.assertEqual(report["validation"]["source_counts"]["component_invocation_rows"], 1)
        self.assertEqual(report["validation"]["source_counts"]["style_keys"], 2)
        self.assertEqual(report["validation"]["source_counts"]["source_contract_rows"], 1)
        self.assertEqual(report["validation"]["style_counts"]["supported"], 2)
        self.assertEqual(report["validation"]["route_counts"]["nodes_total"], 9)
        self.assertEqual(report["validation"]["route_counts"]["route_rows_total"], 1)
        self.assertEqual(report["validation"]["route_counts"]["route_rows_native_cpp"], 1)
        self.assertEqual(report["validation"]["route_counts"]["route_rows_recorder_candidate"], 1)
        self.assertNotIn("js_engine_initialized", report["validation"]["route_counts"])
        self.assertNotIn("requires_runtime_js", report["validation"]["route_counts"])
        self.assertNotIn("behavior_pass", report["validation"]["route_counts"])
        self.assertEqual(report["validation"]["primitive_counts"]["primitive_knob"], 1)
        self.assertEqual(report["validation"]["primitive_counts"]["with_parameter_bindings"], 1)
        self.assertEqual(report["validation"]["primitive_counts"]["with_event_contracts"], 1)
        self.assertTrue(report["validation"]["binary_dependencies"]["js_engine_present"])
        fir.validate_frontend_ir(report)

    def test_builds_report_from_corpus_route_rows_without_explicit_node_ids(self) -> None:
        route_manifest = {
            "schema": "pulp-native-ui-source-node-route-manifest-v1",
            "fixture": "fixture-corpus",
            "inputs": {
                "sourceJsx": {
                    "path": "fixtures/Compressor.tsx",
                    "sha256": "c" * 64,
                },
                "sourceAudit": "generated_inline_by_native_ui_phase_h_seed_route_manifests",
                "sourceAuditSummary": {
                    "schema": "pulp-source-audit-summary-v1",
                    "input": {
                        "bytes": 4096,
                    },
                    "summary": {
                        "jsx_elements": 2,
                        "map_calls": 1,
                        "style_objects": 2,
                        "css_values": 5,
                        "component_counts": {
                            "section": 1,
                            "input": 1,
                        },
                        "state_setters": {
                            "threshold": "setThreshold",
                        },
                    },
                    "materiality": {
                        "event_contracts": 1,
                        "set_state_events": 1,
                    },
                },
            },
            "source_contract_overlay": {
                "source": {
                    "source_of_truth": "archived_corpus_fixture",
                },
                "route_rows": [
                    {
                        "stable_source_path": "fixtures/Compressor.tsx:24:section[0]",
                        "source_component_family": "section",
                        "source_component_name": "section",
                        "source_line": 24,
                        "route_type": "native_layout",
                        "required_native_primitive": "layout",
                        "confidence": 0.75,
                    },
                    {
                        "stable_source_path": "fixtures/Compressor.tsx:48:input[0]",
                        "source_component_family": "input[type=range]",
                        "source_component_name": "input",
                        "source_line": 48,
                        "route_type": "native_cpp",
                        "required_native_primitive": "fader",
                        "state_contracts": [
                            {
                                "kind": "set_state_from_event_value",
                                "state_key": "threshold",
                            }
                        ],
                        "confidence": 0.85,
                    },
                ],
            },
        }

        report = fir.build_frontend_ir(
            route_manifest,
            {},
            pathlib.Path("/repo/reports/route.json"),
            pathlib.Path("/repo"),
        )

        self.assertEqual(report["source"]["source_of_truth"], "archived_fixture")
        self.assertEqual(report["source"]["counts"]["bytes"], 4096)
        self.assertEqual(report["source"]["counts"]["jsx_elements"], 2)
        self.assertEqual(report["source"]["counts"]["map_calls"], 1)
        self.assertEqual(report["source"]["counts"]["style_objects"], 2)
        self.assertEqual(report["source"]["counts"]["css_values"], 5)
        self.assertEqual(report["source"]["counts"]["component_section"], 1)
        self.assertEqual(report["source"]["counts"]["component_input"], 1)
        self.assertEqual(report["source"]["counts"]["state_setters"], 1)
        self.assertEqual(report["source"]["counts"]["materiality_event_contracts"], 1)
        self.assertIn("runtime_array_maps", report["source"]["dynamic_risks"])
        self.assertEqual(report["nodes"][0]["id"], "fixtures/Compressor.tsx:24:section[0]")
        self.assertEqual(report["routes"][0]["chosen_route"], "native_html")
        self.assertEqual(report["routes"][1]["chosen_route"], "native_cpp")
        self.assertEqual(report["nodes"][1]["state"]["local_ui"]["threshold"], "set_state_from_event_value")
        self.assertEqual(report["validation"]["source_counts"]["source_contract_rows"], 2)
        self.assertEqual(report["validation"]["route_counts"]["route_rows_native_html"], 1)
        self.assertEqual(report["validation"]["primitive_counts"]["primitive_layout"], 1)
        self.assertEqual(report["validation"]["primitive_counts"]["with_state_contracts"], 1)
        self.assertIn("did not provide a DesignIR", " ".join(report["validation"]["notes"]))
        fir.validate_frontend_ir(report)

    def test_boolean_values_are_not_numeric_evidence(self) -> None:
        with self.assertRaisesRegex(ValueError, "validation.route_counts.bad"):
            fir.validate_count_map({"bad": True}, "validation.route_counts")

        row = {
            "id": "node.bool",
            "stable_source_path": "fixtures/UI.jsx:1:Widget[0]",
            "source_line": True,
            "required_native_primitive": "control",
            "route_type": "native_cpp",
            "size": True,
            "confidence": True,
        }

        nodes = fir.nodes_from_rows([row])
        routes = fir.routes_from_rows([row])

        self.assertNotIn("line", nodes[0]["source_span"])
        self.assertNotIn("size", nodes[0]["style"]["layout"])
        self.assertEqual(routes[0]["candidate_routes"][0]["confidence"], 0.0)

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

    def test_route_name_normalizes_native_host_and_custom_paint_routes(self) -> None:
        self.assertEqual(fir.route_name("native_host_service"), "native_cpp")
        self.assertEqual(fir.route_name("native_custom_paint"), "recorded_paint")
        self.assertEqual(fir.route_name("native_layout"), "native_html")

    def test_cli_writes_deterministic_json(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            route_manifest_path = root / "reports/route.json"
            output_path = root / "reports/frontend-ir.json"
            write_json(
                route_manifest_path,
                {
                    "schema": "pulp-native-ui-route-manifest-v1",
                    "fixture": "fixture-b",
                    "inputs": {
                        "sourceJsx": {"path": "fixtures/UI.jsx"},
                        "sourceAuditSummary": {
                            "summary": {
                                "jsx_elements": 3,
                                "map_calls": 1,
                            },
                            "materiality": {
                                "event_contracts": 2,
                            },
                        },
                        "ir": {"path": "reports/generated/ui-ir.json"},
                    },
                    "source_contract_overlay": {
                        "node_route_rows": [],
                    },
                },
            )

            rc = fir.main([
                "--route-manifest",
                str(route_manifest_path),
                "--output",
                str(output_path),
                "--repo-root",
                str(root),
            ])

            self.assertEqual(rc, 0)
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(report["schema"], "pulp-frontend-ir-v0")
            self.assertEqual(report["route_manifest"]["path"], "reports/route.json")
            self.assertEqual(report["source"]["counts"]["jsx_elements"], 3)
            self.assertEqual(report["source"]["counts"]["materiality_event_contracts"], 2)
            self.assertIn("runtime_array_maps", report["source"]["dynamic_risks"])
            self.assertEqual(report["routes"], [])


if __name__ == "__main__":
    unittest.main()
