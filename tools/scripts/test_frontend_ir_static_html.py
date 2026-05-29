#!/usr/bin/env python3
"""Tests for tools/scripts/frontend_ir_static_html.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent


def load_module(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, SCRIPT_DIR / filename)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


static_html = load_module("frontend_ir_static_html", "frontend_ir_static_html.py")
fir = load_module("frontend_ir_report", "frontend_ir_report.py")
gate = load_module("frontend_ir_gate", "frontend_ir_gate.py")


class StaticHtmlFrontendIrTests(unittest.TestCase):
    def test_static_html_fixture_emits_frontend_ir_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            fixture_dir = root / "fixtures"
            html_path = fixture_dir / "control-panel.html"
            css_path = fixture_dir / "control-panel.css"
            fixture_dir.mkdir()
            css_path.write_text(
                ".panel { display: flex; gap: 12px; border-radius: 8px; }\n"
                ".primary { color: #e8eef8; }\n",
                encoding="utf-8",
            )
            html_path.write_text(
                """<!doctype html>
<html>
<head>
  <link rel="stylesheet" href="control-panel.css">
  <style>
    .inline { box-shadow: 0 2px 12px #000; }
  </style>
</head>
<body>
  <main class="shell">
    <section class="panel inline" style="padding: 16px; background: #101216">
      <h1>Static Panel</h1>
      <button class="primary">Bypass</button>
      <input type="range" value="0.7">
      <svg width="24" height="24"><path d="M0 12 L24 12"></path></svg>
    </section>
  </main>
</body>
</html>
""",
                encoding="utf-8",
            )

            manifest = static_html.build_route_manifest(html_path, root, "static-html-panel")
            report = fir.build_frontend_ir(manifest, {}, root / "reports/static-html-route-manifest.json", root)
            evidence_gate = gate.gate_frontend_ir(report, "evidence")
            native_gate = gate.gate_frontend_ir(report, "native-readiness")

            self.assertEqual(manifest["schema"], "pulp-static-html-route-manifest-v1")
            self.assertEqual(manifest["inputs"]["sourceHtml"]["path"], "fixtures/control-panel.html")
            self.assertEqual(manifest["inputs"]["styleSheets"][0]["path"], "fixtures/control-panel.css")
            self.assertEqual(manifest["route_metrics"]["js_engine_initialized"], False)
            self.assertEqual(manifest["route_metrics"]["native_html_candidate_node_routes"], 8)
            self.assertEqual(report["source"]["kind"], "html")
            self.assertEqual(report["source"]["source_of_truth"], "archived_fixture")
            self.assertEqual(report["validation"]["source_counts"]["html_elements"], 8)
            self.assertEqual(report["validation"]["source_counts"]["css_rules"], 3)
            self.assertEqual(report["validation"]["source_counts"]["local_stylesheet_resources"], 1)
            self.assertEqual(report["validation"]["style_counts"]["source_css_values"], 7)
            self.assertEqual(report["validation"]["style_counts"]["source_style_attributes"], 1)
            self.assertEqual(report["validation"]["primitive_counts"]["primitive_layout"], 3)
            self.assertEqual(report["validation"]["primitive_counts"]["primitive_button"], 1)
            self.assertEqual(report["validation"]["primitive_counts"]["primitive_fader"], 1)
            self.assertEqual(report["validation"]["primitive_counts"]["primitive_vector"], 2)
            self.assertEqual(report["validation"]["resource_counts"]["total"], 2)
            self.assertEqual(report["validation"]["resource_counts"]["with_sha256"], 2)
            self.assertEqual(report["validation"]["resource_counts"]["with_byte_size"], 2)
            self.assertEqual(report["validation"]["resource_counts"]["watchable"], 2)
            self.assertEqual(report["validation"]["resource_counts"]["route_usage_native_html"], 2)
            self.assertEqual(report["resources"][0]["id"], "input.source_html")
            self.assertEqual(report["resources"][1]["id"], "input.style_sheets")
            self.assertEqual(report["resources"][1]["mime"], "text/css")
            self.assertTrue(report["resources"][0]["watch"])
            self.assertTrue(report["resources"][1]["watch"])
            self.assertNotIn("js_engine_present", report["validation"]["binary_dependencies"])
            self.assertEqual(evidence_gate["verdict"], "ready")
            self.assertEqual(native_gate["verdict"], "not_ready")
            self.assertEqual(
                [item["id"] for item in native_gate["checks"] if item["status"] == "fail"],
                ["binary_no_js_engine", "native_compile_pass"],
            )
            fir.validate_frontend_ir(report)

    def test_cli_writes_route_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            html_path = root / "fixtures/simple.html"
            output_path = root / "reports/simple-route-manifest.json"
            html_path.parent.mkdir()
            html_path.write_text("<main><p style=\"color:#fff\">Hello</p></main>\n", encoding="utf-8")

            rc = static_html.main([
                "--html",
                str(html_path),
                "--fixture",
                "simple",
                "--output",
                str(output_path),
                "--repo-root",
                str(root),
            ])

            self.assertEqual(rc, 0)
            manifest = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(manifest["fixture"], "simple")
            self.assertEqual(manifest["inputs"]["sourceHtml"]["path"], "fixtures/simple.html")
            self.assertEqual(len(manifest["source_contract_overlay"]["route_rows"]), 2)


if __name__ == "__main__":
    unittest.main()
