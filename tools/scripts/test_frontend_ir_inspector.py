#!/usr/bin/env python3
"""Tests for tools/scripts/frontend_ir_inspector.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parent / "frontend_ir_inspector.py"
spec = importlib.util.spec_from_file_location("frontend_ir_inspector", SCRIPT)
assert spec and spec.loader
inspector = importlib.util.module_from_spec(spec)
sys.modules["frontend_ir_inspector"] = inspector
spec.loader.exec_module(inspector)


def sample_report() -> dict:
    return {
        "schema": "pulp-frontend-ir-v0",
        "fixture_id": "panel",
        "source": {
            "kind": "html",
            "path": "fixtures/panel.html",
            "sha256": "a" * 64,
            "source_of_truth": "archived_fixture",
            "counts": {
                "html_elements": 1,
            },
        },
        "resources": [
            {
                "id": "input.source_html",
                "original_uri": "fixtures/panel.html",
                "resolved_uri": "fixtures/panel.html",
                "sha256": "a" * 64,
                "byte_size": 42,
                "mime": "text/html",
                "requested_by": ["panel"],
                "route_usage": ["native_html"],
                "watch": True,
                "transforms": [],
            },
            {
                "id": "input.style_sheets",
                "original_uri": "fixtures/panel.css",
                "resolved_uri": "fixtures/panel.css",
                "sha256": "b" * 64,
                "byte_size": 12,
                "mime": "text/css",
                "requested_by": ["html.0.button"],
                "route_usage": ["native_html"],
                "watch": True,
                "transforms": [],
            },
        ],
        "nodes": [
            {
                "id": "html.0.button",
                "semantic_role": "button",
                "source_span": {
                    "node_id": "html.0.button",
                    "path": "fixtures/panel.html:4:button[0]",
                    "line": 4,
                },
                "style": {
                    "layout": {
                        "size": {
                            "value": 20,
                            "support": {
                                "native_html": "present",
                            },
                        }
                    },
                    "paint_layers": [
                        {
                            "value": "#fff",
                            "support": {
                                "native_html": "present",
                            },
                        }
                    ],
                    "typography": {},
                    "variants": {},
                },
                "state": {
                    "parameters": [{"id": "bypass"}],
                    "meters": [],
                    "local_ui": {"pressed": "state"},
                    "derived": {},
                    "dynamic_risk": [],
                },
                "resources": ["input.style_sheets"],
            }
        ],
        "routes": [
            {
                "node_id": "html.0.button",
                "semantic_role": "button",
                "chosen_route": "native_html",
                "requires_js_engine": False,
                "requires_gpu": True,
                "validation_refs": ["route_manifest:html.0.button"],
            }
        ],
        "validation": {
            "compile": {"status": "not_run"},
            "binary_dependencies": {"js_engine_present": False},
            "screenshots": [],
            "proofs": [],
        },
    }


class FrontendIrInspectorTests(unittest.TestCase):
    def test_builds_resource_log_and_node_cards(self) -> None:
        gate = {
            "schema": "pulp-frontend-ir-gate-v0",
            "mode": "evidence",
            "verdict": "ready",
            "summary": {"checks": 5, "failures": 0},
        }
        session_diff = {
            "schema": "pulp-frontend-ir-session-diff-v0",
            "summary": {
                "recommended_reload": "style_resource_reload",
            },
        }

        report = inspector.build_inspector_report(sample_report(), [gate], session_diff)

        self.assertEqual(report["schema"], "pulp-frontend-ir-inspector-v0")
        self.assertEqual(report["summary"]["nodes"], 1)
        self.assertEqual(report["summary"]["resources"], 2)
        self.assertEqual(report["summary"]["watchable_resources"], 2)
        self.assertEqual(report["summary"]["routes"]["native_html"], 1)
        self.assertEqual(report["route_resource_usage"]["native_html"], ["input.source_html", "input.style_sheets"])
        self.assertEqual(report["resource_log"][1]["id"], "input.style_sheets")
        self.assertEqual(report["nodes"][0]["route"]["chosen_route"], "native_html")
        self.assertEqual(report["nodes"][0]["route"]["validation_refs"], ["route_manifest:html.0.button"])
        self.assertEqual(report["nodes"][0]["style"]["layout_values"], 1)
        self.assertEqual(report["nodes"][0]["style"]["paint_layers"], 1)
        self.assertEqual(report["nodes"][0]["style"]["support_counts"], {"native_html:present": 2})
        self.assertEqual(report["nodes"][0]["state"]["parameters"], 1)
        self.assertEqual(report["nodes"][0]["resources"]["explicit"], ["input.style_sheets"])
        self.assertEqual(report["nodes"][0]["resources"]["requested_by_node"], ["input.style_sheets"])
        self.assertEqual(report["validation"]["gates"][0]["verdict"], "ready")
        self.assertEqual(report["validation"]["session_diff"]["summary"]["recommended_reload"], "style_resource_reload")

    def test_cli_writes_inspector_report(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            frontend_ir = root / "frontend-ir.json"
            gate = root / "gate.json"
            output = root / "inspector.json"
            frontend_ir.write_text(json.dumps(sample_report()), encoding="utf-8")
            gate.write_text(json.dumps({
                "schema": "pulp-frontend-ir-gate-v0",
                "mode": "evidence",
                "verdict": "ready",
                "summary": {},
            }), encoding="utf-8")

            rc = inspector.main([
                "--frontend-ir",
                str(frontend_ir),
                "--gate",
                str(gate),
                "--output",
                str(output),
                "--repo-root",
                str(root),
            ])

            self.assertEqual(rc, 0)
            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(report["frontend_ir"]["path"], "frontend-ir.json")
            self.assertEqual(report["validation"]["gates"][0]["mode"], "evidence")


if __name__ == "__main__":
    unittest.main()
