#!/usr/bin/env python3
"""Tests for tools/scripts/frontend_ir_session.py."""

from __future__ import annotations

import copy
import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parent / "frontend_ir_session.py"
spec = importlib.util.spec_from_file_location("frontend_ir_session", SCRIPT)
assert spec and spec.loader
session = importlib.util.module_from_spec(spec)
sys.modules["frontend_ir_session"] = session
spec.loader.exec_module(session)


def base_report() -> dict:
    return {
        "schema": "pulp-frontend-ir-v0",
        "fixture_id": "static-html-panel",
        "source": {
            "kind": "html",
            "path": "fixtures/control-panel.html",
            "sha256": "a" * 64,
            "source_of_truth": "archived_fixture",
            "counts": {
                "html_elements": 2,
                "source_contract_rows": 2,
            },
        },
        "nodes": [
            {
                "id": "html.0.main",
                "semantic_role": "layout",
                "style": {},
                "state": {},
            },
            {
                "id": "html.1.button",
                "semantic_role": "button",
                "style": {},
                "state": {},
            },
        ],
        "resources": [
            {
                "id": "input.source_html",
                "original_uri": "fixtures/control-panel.html",
                "resolved_uri": "fixtures/control-panel.html",
                "sha256": "a" * 64,
                "byte_size": 100,
                "mime": "text/html",
                "requested_by": ["static-html-panel"],
                "route_usage": ["native_html"],
                "watch": True,
                "transforms": [],
            },
            {
                "id": "input.style_sheets",
                "original_uri": "fixtures/control-panel.css",
                "resolved_uri": "fixtures/control-panel.css",
                "sha256": "b" * 64,
                "byte_size": 40,
                "mime": "text/css",
                "requested_by": ["static-html-panel"],
                "route_usage": ["native_html"],
                "watch": True,
                "transforms": [],
            },
        ],
        "tokens": {},
        "tweaks": [],
        "routes": [
            {
                "node_id": "html.0.main",
                "semantic_role": "layout",
                "chosen_route": "native_html",
                "requires_js_engine": False,
            },
            {
                "node_id": "html.1.button",
                "semantic_role": "button",
                "chosen_route": "native_html",
                "requires_js_engine": False,
            },
        ],
        "validation": {
            "style_counts": {
                "source_css_values": 4,
            },
            "route_counts": {
                "route_rows_native_html": 2,
            },
            "primitive_counts": {
                "primitive_layout": 1,
                "primitive_button": 1,
            },
            "compile": {
                "status": "not_run",
            },
            "binary_dependencies": {
                "js_engine_present": False,
            },
            "screenshots": [],
        },
    }


class FrontendIrSessionTests(unittest.TestCase):
    def test_stylesheet_resource_change_is_narrow_style_resource_reload(self) -> None:
        before = base_report()
        after = copy.deepcopy(before)
        after["resources"][1]["sha256"] = "c" * 64
        after["resources"][1]["byte_size"] = 42

        report = session.compare_reports(before, after)

        self.assertEqual(report["summary"]["reload_scope"], ["style_contract", "resources"])
        self.assertEqual(report["summary"]["recommended_reload"], "style_resource_reload")
        self.assertTrue(report["summary"]["component_classification_preserved"])
        self.assertTrue(report["summary"]["narrow_reload_safe"])
        self.assertTrue(report["changes"]["style_contract"]["style_resource_changed"])
        self.assertEqual(report["changes"]["resources"]["changed"][0]["id"], "input.style_sheets")

    def test_token_only_change_is_narrow_token_reload(self) -> None:
        before = base_report()
        after = copy.deepcopy(before)
        after["tokens"] = {
            "colors.accent": {
                "type": "color",
                "value": "#fff",
                "resolved_value": "#fff",
            }
        }

        report = session.compare_reports(before, after)

        self.assertEqual(report["summary"]["reload_scope"], ["tokens"])
        self.assertEqual(report["summary"]["recommended_reload"], "token_tweak_reload")
        self.assertTrue(report["summary"]["narrow_reload_safe"])
        self.assertEqual(report["changes"]["tokens"]["after_count"], 1)

    def test_route_change_requires_full_reimport(self) -> None:
        before = base_report()
        after = copy.deepcopy(before)
        after["routes"][1]["chosen_route"] = "live_js"
        after["routes"][1]["requires_js_engine"] = True
        after["validation"]["route_counts"] = {
            "route_rows_native_html": 1,
            "route_rows_live_js": 1,
        }

        report = session.compare_reports(before, after)

        self.assertIn("routes", report["summary"]["reload_scope"])
        self.assertEqual(report["summary"]["recommended_reload"], "full_reimport")
        self.assertFalse(report["summary"]["component_classification_preserved"])
        self.assertFalse(report["summary"]["narrow_reload_safe"])

    def test_cli_writes_session_diff(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            before = root / "before.json"
            after = root / "after.json"
            output = root / "session-diff.json"
            before_report = base_report()
            after_report = copy.deepcopy(before_report)
            after_report["resources"][1]["sha256"] = "c" * 64
            before.write_text(json.dumps(before_report), encoding="utf-8")
            after.write_text(json.dumps(after_report), encoding="utf-8")

            rc = session.main([
                "--before",
                str(before),
                "--after",
                str(after),
                "--output",
                str(output),
                "--repo-root",
                str(root),
            ])

            self.assertEqual(rc, 0)
            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(report["schema"], "pulp-frontend-ir-session-diff-v0")
            self.assertEqual(report["before"]["path"], "before.json")
            self.assertEqual(report["after"]["path"], "after.json")
            self.assertEqual(report["summary"]["recommended_reload"], "style_resource_reload")


if __name__ == "__main__":
    unittest.main()
