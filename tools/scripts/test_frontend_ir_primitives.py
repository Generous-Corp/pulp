#!/usr/bin/env python3
"""Tests for tools/scripts/frontend_ir_primitives.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parent / "frontend_ir_primitives.py"
spec = importlib.util.spec_from_file_location("frontend_ir_primitives", SCRIPT)
assert spec and spec.loader
primitives = importlib.util.module_from_spec(spec)
sys.modules["frontend_ir_primitives"] = primitives
spec.loader.exec_module(primitives)


def sample_report() -> dict:
    return {
        "schema": "pulp-frontend-ir-v0",
        "fixture_id": "panel",
        "nodes": [
            {
                "id": "html.0.main",
                "semantic_role": "main",
                "source_span": {"node_id": "html.0.main", "path": "panel.html:1:main[0]"},
                "style": {
                    "layout": {
                        "size": {"value": 12, "provenance": "inline_style"},
                    },
                    "paint_layers": [],
                    "typography": {},
                    "variants": {},
                },
                "state": {},
            },
            {
                "id": "html.1.button",
                "semantic_role": "button",
                "source_span": {"node_id": "html.1.button", "path": "panel.html:2:button[0]"},
                "style": {
                    "layout": {},
                    "paint_layers": [{"value": "#fff", "provenance": "inline_style"}],
                    "typography": {},
                    "variants": {},
                },
                "state": {
                    "parameters": [{"id": "bypass"}],
                    "meters": [],
                    "local_ui": {},
                    "derived": {},
                    "dynamic_risk": [],
                },
            },
            {
                "id": "custom.0",
                "semantic_role": "custom-widget",
                "style": {},
                "state": {},
            },
        ],
        "routes": [
            {"node_id": "html.0.main", "chosen_route": "native_html", "requires_js_engine": False},
            {"node_id": "html.1.button", "chosen_route": "native_cpp", "requires_js_engine": False},
            {"node_id": "custom.0", "chosen_route": "live_js", "requires_js_engine": True},
        ],
        "validation": {
            "source_counts": {"source_contract_rows": 3},
            "primitive_counts": {
                "primitive_layout": 1,
                "primitive_button": 1,
                "primitive_custom_widget": 1,
            },
        },
    }


class FrontendIrPrimitiveTests(unittest.TestCase):
    def test_builds_primitive_coverage_report(self) -> None:
        report = primitives.build_primitive_report(sample_report())

        self.assertEqual(report["schema"], "pulp-frontend-ir-primitive-coverage-v0")
        self.assertEqual(report["summary"]["nodes"], 3)
        self.assertEqual(report["summary"]["observed_primitives"], 3)
        self.assertEqual(report["summary"]["covered_catalog_primitives"], 2)
        self.assertEqual(report["summary"]["observed_missing_from_catalog"], ["custom_widget"])
        self.assertEqual(report["summary"]["nodes_with_source_span"], 2)
        self.assertEqual(report["summary"]["nodes_with_style"], 2)
        self.assertEqual(report["summary"]["nodes_with_binding"], 1)
        self.assertEqual(report["summary"]["nodes_requiring_js"], 1)
        by_role = {row["role"]: row for row in report["primitives"]}
        self.assertEqual(by_role["layout"]["category"], "layout")
        self.assertEqual(by_role["button"]["observed_routes"], {"native_cpp": 1})
        self.assertEqual(by_role["button"]["nodes_with_binding"], 1)
        self.assertFalse(by_role["custom_widget"]["in_catalog"])

    def test_cli_writes_primitive_coverage_report(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            frontend_ir = root / "frontend-ir.json"
            output = root / "primitive-coverage.json"
            frontend_ir.write_text(json.dumps(sample_report()), encoding="utf-8")

            rc = primitives.main([
                "--frontend-ir",
                str(frontend_ir),
                "--output",
                str(output),
                "--repo-root",
                str(root),
            ])

            self.assertEqual(rc, 0)
            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(report["frontend_ir"]["path"], "frontend-ir.json")
            self.assertEqual(report["summary"]["observed_missing_from_catalog"], ["custom_widget"])


if __name__ == "__main__":
    unittest.main()
