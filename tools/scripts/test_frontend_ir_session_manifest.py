#!/usr/bin/env python3
"""Tests for tools/scripts/frontend_ir_session_manifest.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parent / "frontend_ir_session_manifest.py"
spec = importlib.util.spec_from_file_location("frontend_ir_session_manifest", SCRIPT)
assert spec and spec.loader
manifest = importlib.util.module_from_spec(spec)
sys.modules["frontend_ir_session_manifest"] = manifest
spec.loader.exec_module(manifest)


def sample_report(root: pathlib.Path) -> dict:
    source = root / "fixtures/panel.html"
    style = root / "fixtures/panel.css"
    route_manifest = root / "reports/route-manifest.json"
    design_ir = root / "reports/design-ir.json"
    proof = root / "reports/proof.json"
    source.parent.mkdir(parents=True, exist_ok=True)
    route_manifest.parent.mkdir(parents=True, exist_ok=True)
    source.write_text("<main><button>Run</button></main>\n", encoding="utf-8")
    style.write_text("button { color: red; }\n", encoding="utf-8")
    route_manifest.write_text('{"schema":"route"}\n', encoding="utf-8")
    design_ir.write_text('{"schema":"design"}\n', encoding="utf-8")
    proof.write_text('{"schema":"proof"}\n', encoding="utf-8")
    return {
        "schema": "pulp-frontend-ir-v0",
        "fixture_id": "panel",
        "source": {
            "kind": "html",
            "path": "fixtures/panel.html",
            "source_of_truth": "archived_fixture",
            "sha256": manifest.sha256_file(source),
            "counts": {"html_elements": 2},
        },
        "design_ir": {
            "kind": "design_ir",
            "path": "reports/design-ir.json",
            "schema": "pulp-design-ir-v1",
        },
        "route_manifest": {
            "kind": "route_manifest",
            "path": "reports/route-manifest.json",
            "schema": "pulp-route-manifest-v1",
        },
        "resources": [
            {
                "id": "input.style_sheets",
                "original_uri": "fixtures/panel.css",
                "resolved_uri": "fixtures/panel.css",
                "sha256": manifest.sha256_file(style),
                "byte_size": style.stat().st_size,
                "mime": "text/css",
                "requested_by": ["panel"],
                "route_usage": ["native_html"],
                "watch": True,
                "transforms": [],
            }
        ],
        "tokens": {
            "colors.accent": {
                "type": "color",
                "value": "#f00",
                "resolved_value": "#f00",
            }
        },
        "tweaks": [
            {
                "node_id": "html.0.button",
                "property": "tokens.color.accent",
                "value": "#0f0",
                "invalidates": ["style"],
                "classification_preserved": True,
            }
        ],
        "routes": [
            {
                "node_id": "html.0.button",
                "chosen_route": "native_html",
                "requires_js_engine": False,
            }
        ],
        "validation": {
            "compile": {
                "status": "pass",
                "proof_artifact": {
                    "kind": "native_proof",
                    "path": "reports/proof.json",
                },
            },
            "binary_dependencies": {
                "js_engine_present": False,
                "proof_artifact": {
                    "kind": "native_proof",
                    "path": "reports/proof.json",
                },
            },
            "proofs": [
                {
                    "kind": "compile_proof",
                    "path": "reports/proof.json",
                    "status": "pass",
                }
            ],
            "screenshots": [],
        },
    }


class FrontendIrSessionManifestTests(unittest.TestCase):
    def test_builds_watchable_resource_session_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            frontend_ir = root / "reports/frontend-ir.json"
            gate = root / "reports/evidence-gate.json"
            inspector = root / "reports/inspector.json"
            session_diff = root / "reports/session-diff.json"
            tweak_sidecar = root / "reports/pulp-tweaks.json"
            report = sample_report(root)
            frontend_ir.write_text(json.dumps(report), encoding="utf-8")
            gate.write_text(json.dumps({
                "schema": "pulp-frontend-ir-gate-v0",
                "mode": "evidence",
                "verdict": "ready",
                "summary": {"checks": 5, "failures": 0},
            }), encoding="utf-8")
            inspector.write_text(json.dumps({
                "schema": "pulp-frontend-ir-inspector-v0",
                "summary": {"nodes": 1},
            }), encoding="utf-8")
            session_diff.write_text(json.dumps({
                "schema": "pulp-frontend-ir-session-diff-v0",
                "summary": {
                    "recommended_reload": "token_tweak_reload",
                    "narrow_reload_safe": True,
                },
            }), encoding="utf-8")
            tweak_sidecar.write_text(json.dumps({"schema": "pulp-tweaks-v0", "tweaks": []}), encoding="utf-8")

            result = manifest.build_session_manifest(
                report,
                frontend_ir,
                root,
                gates=[(gate, json.loads(gate.read_text(encoding="utf-8")))],
                inspector=(inspector, json.loads(inspector.read_text(encoding="utf-8"))),
                session_diff=(session_diff, json.loads(session_diff.read_text(encoding="utf-8"))),
                tweak_sidecars=[tweak_sidecar],
            )

            self.assertEqual(result["schema"], "pulp-frontend-ir-session-v0")
            self.assertEqual(result["summary"]["resources"], 1)
            self.assertEqual(result["summary"]["routes"]["by_route"], {"native_html": 1})
            self.assertEqual(result["summary"]["tokens"]["resolved"], 1)
            self.assertEqual(result["summary"]["tweaks"]["invalidations"], {"style": 1})
            self.assertEqual(result["reload_policy"]["current_recommendation"], "token_tweak_reload")
            self.assertTrue(result["reload_policy"]["narrow_reload_safe"])
            self.assertEqual(result["validation"]["gates"][0]["verdict"], "ready")
            self.assertEqual(result["validation"]["inspector"], {"nodes": 1})
            self.assertIn("fixtures/panel.html", result["watch"]["paths"])
            self.assertIn("fixtures/panel.css", result["watch"]["paths"])
            self.assertIn("reports/route-manifest.json", result["watch"]["paths"])
            self.assertIn("reports/pulp-tweaks.json", result["watch"]["paths"])
            self.assertEqual(result["watch"]["resource_ids"], ["input.style_sheets"])
            artifact_ids = {entry["id"] for entry in result["artifacts"]}
            self.assertIn("design_ir", artifact_ids)
            self.assertIn("frontend_ir", artifact_ids)
            self.assertIn("proof.0", artifact_ids)

    def test_cli_writes_session_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            frontend_ir = root / "frontend-ir.json"
            output = root / "session.json"
            frontend_ir.write_text(json.dumps(sample_report(root)), encoding="utf-8")

            rc = manifest.main([
                "--frontend-ir",
                str(frontend_ir),
                "--output",
                str(output),
                "--repo-root",
                str(root),
            ])

            self.assertEqual(rc, 0)
            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(report["schema"], "pulp-frontend-ir-session-v0")
            self.assertIn("proof.0", {entry["id"] for entry in report["artifacts"]})


if __name__ == "__main__":
    unittest.main()
