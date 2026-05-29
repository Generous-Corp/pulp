#!/usr/bin/env python3
"""Tests for tools/scripts/frontend_ir_report.py."""

from __future__ import annotations

import importlib.util
import hashlib
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
                                "module": "OSC",
                                "param": "gain",
                            }
                        ],
                        "initial_value": 0.25,
                        "value": 0.5,
                        "default_value": 0.0,
                        "default_value_source": "prop.default",
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
        self.assertEqual(report["nodes"][0]["state"]["parameters"][0]["source_binding_id"], "binding.gain")
        self.assertEqual(report["nodes"][0]["state"]["parameters"][0]["module"], "OSC")
        self.assertEqual(report["nodes"][0]["state"]["parameters"][0]["param"], "gain")
        self.assertEqual(report["nodes"][0]["state"]["parameters"][0]["initial_value"], 0.25)
        self.assertEqual(report["nodes"][0]["state"]["parameters"][0]["value"], 0.5)
        self.assertEqual(report["nodes"][0]["state"]["parameters"][0]["range"]["default"], 0.0)
        self.assertEqual(report["nodes"][0]["state"]["parameters"][0]["default_source"], "prop.default")
        self.assertEqual(report["tokens"]["colors.accent"]["type"], "reference")
        self.assertEqual(report["tokens"]["colors.accent"]["value"], "colors.accent")
        self.assertEqual(report["tokens"]["colors.accent"]["source_identity"]["provenance"],
                         "style_token_references")
        self.assertEqual(report["validation"]["token_counts"]["total"], 1)
        self.assertEqual(report["validation"]["token_counts"]["unresolved"], 1)
        self.assertEqual(report["validation"]["token_counts"]["referenced_by_rows"], 1)
        self.assertEqual(report["validation"]["tweak_counts"]["total"], 0)
        self.assertEqual(report["routes"][0]["chosen_route"], "native_cpp")
        self.assertFalse(report["routes"][0]["requires_js_engine"])
        self.assertIn("validation_refs", report["routes"][0])
        self.assertEqual(report["validation"]["source_counts"]["component_invocations"], 1)
        self.assertEqual(report["validation"]["source_counts"]["component_invocation_rows"], 1)
        self.assertEqual(report["validation"]["source_counts"]["style_keys"], 2)
        self.assertEqual(report["validation"]["source_counts"]["source_contract_rows"], 1)
        self.assertEqual(report["validation"]["style_counts"]["supported"], 2)
        self.assertEqual(report["validation"]["style_counts"]["support_native_cpp_present"], 2)
        self.assertEqual(report["validation"]["style_counts"]["support_live_js_planned"], 2)
        self.assertNotIn("source_css_values", report["validation"]["style_counts"])
        self.assertEqual(report["validation"]["style_counts"]["source_style_attributes"], 3)
        self.assertEqual(report["validation"]["style_counts"]["source_style_keys"], 2)
        self.assertEqual(report["validation"]["state_counts"]["parameters"], 1)
        self.assertEqual(report["validation"]["state_counts"]["parameters_with_value"], 1)
        self.assertEqual(report["validation"]["state_counts"]["parameters_with_initial_value"], 1)
        self.assertEqual(report["validation"]["state_counts"]["parameters_with_default"], 1)
        self.assertEqual(report["validation"]["state_counts"]["parameters_with_source_binding_id"], 1)
        self.assertEqual(report["validation"]["state_counts"]["parameters_with_module_param"], 1)
        self.assertEqual(report["validation"]["state_counts"]["meters"], 0)
        self.assertEqual(report["validation"]["state_counts"]["local_ui_state_keys"], 0)
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
        self.assertEqual([resource["id"] for resource in report["resources"]],
                         ["input.source_jsx", "input.ir"])
        self.assertEqual(report["resources"][0]["route_usage"], ["native_cpp"])
        self.assertEqual(report["resources"][1]["route_usage"], ["native_cpp"])
        self.assertTrue(report["resources"][0]["watch"])
        self.assertFalse(report["resources"][1]["watch"])
        self.assertEqual(report["validation"]["resource_counts"]["total"], 2)
        self.assertEqual(report["validation"]["resource_counts"]["with_sha256"], 2)
        self.assertEqual(report["validation"]["resource_counts"]["watchable"], 1)
        self.assertEqual(report["validation"]["resource_counts"]["route_usage_native_cpp"], 2)
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
                        "conditional_style_values": 1,
                    },
                },
            },
            "route_metrics": {
                "js_engine_initialized": False,
                "requires_runtime_js": False,
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
        self.assertEqual(report["validation"]["style_counts"]["source_css_values"], 5)
        self.assertEqual(report["validation"]["style_counts"]["source_style_objects"], 2)
        self.assertEqual(report["validation"]["style_counts"]["source_conditional_style_values"], 1)
        self.assertNotIn("support_native_html_present", report["validation"]["style_counts"])
        self.assertEqual(report["validation"]["state_counts"]["parameters"], 0)
        self.assertEqual(report["validation"]["state_counts"]["local_ui_state_keys"], 1)
        self.assertEqual(report["validation"]["route_counts"]["route_rows_native_html"], 1)
        self.assertEqual(report["validation"]["primitive_counts"]["primitive_layout"], 1)
        self.assertEqual(report["validation"]["primitive_counts"]["with_state_contracts"], 1)
        self.assertEqual([resource["id"] for resource in report["resources"]], ["input.source_jsx"])
        self.assertEqual(report["resources"][0]["route_usage"], ["native_cpp", "native_html"])
        self.assertEqual(report["validation"]["resource_counts"]["total"], 1)
        self.assertEqual(report["validation"]["resource_counts"]["route_usage_native_cpp"], 1)
        self.assertEqual(report["validation"]["resource_counts"]["route_usage_native_html"], 1)
        self.assertEqual(report["validation"]["token_counts"]["total"], 0)
        self.assertEqual(report["validation"]["tweak_counts"]["total"], 0)
        self.assertNotIn("js_engine_present", report["validation"]["binary_dependencies"])
        self.assertIn("did not provide a DesignIR", " ".join(report["validation"]["notes"]))
        fir.validate_frontend_ir(report)

    def test_cli_attaches_tweak_sidecar_without_source_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            source = root / "fixtures/UI.jsx"
            source.parent.mkdir(parents=True)
            source.write_text("export default function UI() { return null; }\n", encoding="utf-8")
            route_manifest = root / "reports/route.json"
            output = root / "reports/frontend-ir.json"
            sidecar = root / "reports/pulp-tweaks.json"
            write_json(route_manifest, {
                "schema": "pulp-native-ui-route-manifest-v1",
                "fixture": "fixture-tweak",
                "inputs": {
                    "sourceJsx": {
                        "path": "fixtures/UI.jsx",
                    },
                },
                "source_contract_overlay": {
                    "source": {
                        "source_of_truth": "archived_fixture",
                    },
                    "node_route_rows": [
                        {
                            "id": "button.1",
                            "stable_source_path": "fixtures/UI.jsx:1:button[0]",
                            "source_line": 1,
                            "required_native_primitive": "button",
                            "route_type": "native_cpp",
                        }
                    ],
                },
            })
            write_json(sidecar, {
                "schema": "pulp-tweaks-v0",
                "tweaks": [
                    {
                        "node_id": "button.1",
                        "property": "tokens.color.accent",
                        "value": "#ff6600",
                    }
                ],
            })

            rc = fir.main([
                "--route-manifest",
                str(route_manifest),
                "--tweaks",
                str(sidecar),
                "--output",
                str(output),
                "--repo-root",
                str(root),
            ])

            self.assertEqual(rc, 0)
            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(report["tweaks"][0]["node_id"], "button.1")
            self.assertEqual(report["tweaks"][0]["invalidates"], ["style"])
            self.assertTrue(report["tweaks"][0]["classification_preserved"])
            self.assertEqual(report["validation"]["tweak_counts"]["total"], 1)
            self.assertEqual(report["validation"]["tweak_counts"]["classification_preserved"], 1)
            self.assertEqual(report["validation"]["tweak_counts"]["invalidates_style"], 1)
            self.assertIn("attached 1 tweak sidecar edits", " ".join(report["validation"]["notes"]))

    def test_builds_report_from_generic_source_file_input(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            source_path = root / "fixtures/static-panel.html"
            source_path.parent.mkdir()
            source_path.write_text("<main><button>Run</button></main>\n", encoding="utf-8")
            route_manifest = {
                "schema": "pulp-generic-source-route-manifest-v1",
                "fixture": "fixture-source-file",
                "inputs": {
                    "sourceFile": "fixtures/static-panel.html",
                    "sourceAuditSummary": {
                        "input": {
                            "bytes": source_path.stat().st_size,
                        },
                        "summary": {
                            "html_elements": 2,
                        },
                    },
                },
                "source_contract_overlay": {
                    "source": {
                        "source_of_truth": "local_file",
                    },
                    "route_rows": [
                        {
                            "stable_source_path": "fixtures/static-panel.html:1:main[0]",
                            "source_line": 1,
                            "route_type": "native_html",
                            "required_native_primitive": "layout",
                        },
                        {
                            "stable_source_path": "fixtures/static-panel.html:1:button[0]",
                            "source_line": 1,
                            "route_type": "native_html",
                            "required_native_primitive": "button",
                        },
                    ],
                },
            }

            report = fir.build_frontend_ir(
                route_manifest,
                {},
                root / "reports/route.json",
                root,
            )

            self.assertEqual(report["source"]["kind"], "html")
            self.assertEqual(report["source"]["path"], "fixtures/static-panel.html")
            self.assertEqual(report["validation"]["source_counts"]["html_elements"], 2)
            self.assertEqual(report["resources"][0]["id"], "input.source_file")
            self.assertTrue(report["resources"][0]["watch"])
            self.assertEqual(report["resources"][0]["byte_size"], source_path.stat().st_size)
            self.assertEqual(report["validation"]["resource_counts"]["route_usage_native_html"], 1)
            fir.validate_frontend_ir(report)

    def test_source_input_accepts_html_dict_and_source_file_string(self) -> None:
        self.assertEqual(
            fir.source_input({
                "inputs": {
                    "sourceHtml": {
                        "path": "fixtures/panel.html",
                        "sha256": "d" * 64,
                    },
                },
            }),
            {
                "path": "fixtures/panel.html",
                "sha256": "d" * 64,
            },
        )
        self.assertEqual(
            fir.source_input({
                "inputs": {
                    "sourceFile": "fixtures/panel.html",
                },
            }),
            {
                "path": "fixtures/panel.html",
            },
        )

    def test_resolves_static_source_token_object_references(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            source_path = root / "fixtures/UI.jsx"
            source_path.parent.mkdir()
            source_path.write_text(
                "const colors = { accent: '#ff6600', textDim: '#666' };\n"
                "export default function UI() { return null; }\n",
                encoding="utf-8",
            )
            route_manifest = {
                "schema": "pulp-native-ui-route-manifest-v1",
                "fixture": "fixture-token",
                "inputs": {
                    "sourceJsx": {
                        "path": "fixtures/UI.jsx",
                    },
                },
                "source_contract_overlay": {
                    "node_route_rows": [
                        {
                            "id": "knob.1",
                            "stable_source_path": "fixtures/UI.jsx:2:Knob[0]",
                            "source_line": 2,
                            "required_native_primitive": "knob",
                            "route_type": "native_cpp",
                            "style_token_references": ["colors.accent", "colors.textDim"],
                        },
                    ],
                },
            }

            report = fir.build_frontend_ir(
                route_manifest,
                {},
                root / "reports/route.json",
                root,
            )

            self.assertEqual(report["tokens"]["colors.accent"]["resolved_value"], "#ff6600")
            self.assertEqual(report["tokens"]["colors.accent"]["resolved_type"], "color")
            self.assertEqual(report["tokens"]["colors.accent"]["source_identity"]["resolved_from"],
                             "source_static_const_object")
            self.assertEqual(report["tokens"]["colors.accent"]["source_identity"]["source_object"], "colors")
            self.assertEqual(report["tokens"]["colors.textDim"]["resolved_value"], "#666")
            self.assertEqual(report["validation"]["token_counts"]["total"], 2)
            self.assertEqual(report["validation"]["token_counts"]["unresolved"], 0)
            self.assertIn("resolved 2 token references", " ".join(report["validation"]["notes"]))
            fir.validate_frontend_ir(report)

    def test_boolean_values_are_not_numeric_evidence(self) -> None:
        with self.assertRaisesRegex(ValueError, "validation.route_counts.bad"):
            fir.validate_count_map({"bad": True}, "validation.route_counts")
        with self.assertRaisesRegex(ValueError, "validation.state_counts.bad"):
            fir.validate_frontend_ir({
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
                "routes": [],
                "validation": {
                    "source_counts": {},
                    "style_counts": {},
                    "state_counts": {"bad": True},
                },
            })
        with self.assertRaisesRegex(ValueError, "tokens.bad.value is required"):
            fir.validate_frontend_ir({
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
                "tokens": {
                    "bad": {
                        "type": "reference",
                    },
                },
                "routes": [],
                "validation": {
                    "source_counts": {},
                    "style_counts": {},
                },
            })

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
            (root / "fixtures").mkdir()
            (root / "fixtures/UI.jsx").write_text("export default function UI() { return null; }\n", encoding="utf-8")
            (root / "reports/generated").mkdir(parents=True)
            (root / "reports/generated/ui-ir.json").write_text("{}\n", encoding="utf-8")
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
            self.assertEqual(report["validation"]["resource_counts"]["with_byte_size"], 2)
            self.assertEqual(report["validation"]["resource_counts"]["with_sha256"], 2)
            self.assertEqual(report["resources"][0]["byte_size"], len("export default function UI() { return null; }\n"))
            self.assertEqual(report["resources"][0]["sha256"],
                             hashlib.sha256(b"export default function UI() { return null; }\n").hexdigest())
            self.assertEqual(report["resources"][0]["mime"], "text/jsx")
            self.assertEqual(report["resources"][1]["sha256"], hashlib.sha256(b"{}\n").hexdigest())
            self.assertEqual(report["resources"][1]["mime"], "application/json")
            self.assertEqual(report["routes"], [])

    def test_cli_applies_phase_g_cpp_only_native_proof(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            route_manifest_path = root / "reports/route.json"
            proof_path = root / "reports/native-proof.json"
            output_path = root / "reports/frontend-ir.json"
            (root / "fixtures").mkdir()
            (root / "fixtures/UI.jsx").write_text("export default function UI() { return null; }\n", encoding="utf-8")
            (root / "reports/generated").mkdir(parents=True)
            (root / "reports/generated/ui-ir.json").write_text("{}\n", encoding="utf-8")
            write_json(
                route_manifest_path,
                {
                    "schema": "pulp-native-ui-route-manifest-v1",
                    "fixture": "fixture-proof",
                    "inputs": {
                        "sourceJsx": {"path": "fixtures/UI.jsx"},
                        "ir": {"path": "reports/generated/ui-ir.json"},
                    },
                    "route_metrics": {
                        "js_engine_initialized": True,
                    },
                    "source_contract_overlay": {
                        "node_route_rows": [
                            {
                                "id": "knob.1",
                                "stable_source_path": "fixtures/UI.jsx:10:Knob[0]",
                                "source_line": 10,
                                "required_native_primitive": "knob",
                                "route_type": "native_cpp",
                            },
                        ],
                    },
                },
            )
            write_json(
                proof_path,
                {
                    "schema": "pulp-native-ui-phase-g-cpp-only-audit-v1",
                    "fixture": "fixture-proof",
                    "target": "pulp-test-design-import-cpp-only",
                    "binary": {
                        "path": "build-cpu/test/pulp-test-design-import-cpp-only",
                        "exists": True,
                        "bytes": 1234,
                        "sha256": "c" * 64,
                    },
                    "cmake": {
                        "links_view_core_only": True,
                        "cpp_only_flag_present": True,
                    },
                    "criteria": {
                        "target_built": True,
                        "cpp_only_compile_flag_present": True,
                        "target_links_view_core_only": True,
                        "binary_has_no_forbidden_script_symbols": True,
                        "generated_source_has_no_script_symbol_needles": True,
                        "js_engine_unavailable_in_target": True,
                    },
                    "phase_g_cpp_only_proven": True,
                },
            )

            rc = fir.main([
                "--route-manifest",
                str(route_manifest_path),
                "--native-proof",
                str(proof_path),
                "--output",
                str(output_path),
                "--repo-root",
                str(root),
            ])

            self.assertEqual(rc, 0)
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(report["validation"]["compile"]["status"], "pass")
            self.assertEqual(report["validation"]["compile"]["source"], "phase_g_cpp_only")
            self.assertFalse(report["validation"]["binary_dependencies"]["js_engine_present"])
            self.assertEqual(report["validation"]["binary_dependencies"]["source"], "phase_g_cpp_only")
            self.assertTrue(report["validation"]["binary_dependencies"]["target_links_view_core_only"])
            self.assertTrue(report["validation"]["binary_dependencies"]["cpp_only_flag_present"])
            self.assertEqual(len(report["validation"]["proofs"]), 2)
            self.assertIn("native_proof:reports/native-proof.json", report["routes"][0]["validation_refs"])

    def test_cli_applies_matching_phase_h_compile_probe(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            route_manifest_path = root / "reports/route.json"
            proof_path = root / "reports/compile-probe.json"
            output_path = root / "reports/frontend-ir.json"
            write_json(
                route_manifest_path,
                {
                    "schema": "pulp-native-ui-source-node-route-manifest-v1",
                    "fixture": "planning:fixtures:compressor-strip",
                    "inputs": {
                        "sourceJsx": {"path": "fixtures/compressor-strip.tsx"},
                    },
                    "source_contract_overlay": {
                        "route_rows": [
                            {
                                "stable_source_path": "fixtures/compressor-strip.tsx:48:input[0]",
                                "source_line": 48,
                                "route_type": "native_cpp",
                                "required_native_primitive": "fader",
                            },
                        ],
                    },
                },
            )
            write_json(
                proof_path,
                {
                    "schema": "pulp-native-ui-phase-h-import-cpp-compile-probe-v1",
                    "rows": [
                        {
                            "fixture_id": "planning:fixtures:compressor-strip",
                            "path": "fixtures/compressor-strip.tsx",
                            "source_cpp": "reports/generated/compressor-strip.cpp",
                            "source_header": "reports/generated/compressor-strip.hpp",
                            "object_path": "/tmp/compressor-strip.o",
                            "exit_code": 0,
                            "object_bytes": 4096,
                            "status": "pass",
                        },
                    ],
                },
            )

            rc = fir.main([
                "--route-manifest",
                str(route_manifest_path),
                "--native-proof",
                str(proof_path),
                "--output",
                str(output_path),
                "--repo-root",
                str(root),
            ])

            self.assertEqual(rc, 0)
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(report["validation"]["compile"]["status"], "pass")
            self.assertEqual(report["validation"]["compile"]["source"], "phase_h_import_cpp_compile_probe")
            self.assertEqual(report["validation"]["compile"]["object_bytes"], 4096)
            self.assertEqual(len(report["validation"]["proofs"]), 1)
            self.assertIn("native_proof:reports/compile-probe.json", report["routes"][0]["validation_refs"])


if __name__ == "__main__":
    unittest.main()
