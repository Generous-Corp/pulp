#!/usr/bin/env python3
"""Facade-level desktop publish report integration tests."""

from __future__ import annotations

import json
import pathlib
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(
        MODULE_PATH,
        module_name="pulp_local_ci_desktop_publish_report_integration",
        add_module_dir=True,
    )


class DesktopPublishReportIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_desktop_publish_report_rollup_edges(self) -> None:
        config = {
            "desktop_automation": {
                "artifact_root": str(self.root / "desktop-artifacts"),
                "publish_mode": "local",
                "publish_branch": "dev-artifacts",
            }
        }
        with self.assertRaisesRegex(ValueError, "at least one run manifest"):
            self.mod.stage_desktop_publish_report(config, [])

        bundle = self.root / "bundle"
        bundle.mkdir()
        (bundle / "manifest.json").write_text('{"label":"bundle-copy"}\n')
        stdout_log = bundle / "stdout.log"
        stdout_log.write_text("hello\n")
        manifest = {
            "target": "mac<>",
            "action": "inspect",
            "label": "UI & Smoke",
            "completed_at": "2026-05-22T12:00:00+00:00",
            "artifacts": {
                "bundle_dir": str(bundle),
                "stdout": str(stdout_log),
                "screenshot": str(bundle / "missing.png"),
                "image_change": {"changed": False},
            },
            "interaction": {"mode": "dom"},
        }

        output_dir = self.root / "desktop-artifacts" / "_published" / "20260522-gallery"
        report = self.mod.stage_desktop_publish_report(config, [manifest], output_dir=output_dir, label="Gallery <One>")

        self.assertEqual(report["label"], "Gallery <One>")
        self.assertEqual(report["run_count"], 1)
        self.assertTrue((output_dir / "index.html").is_file())
        self.assertTrue((output_dir / "index.json").is_file())
        payload = json.loads((output_dir / "index.json").read_text())
        published_run = payload["runs"][0]
        self.assertEqual(payload["publish_mode"], "local")
        self.assertEqual(payload["publish_branch"], "dev-artifacts")
        self.assertEqual(published_run["target"], "mac<>")
        self.assertEqual(published_run["interaction_mode"], "dom")
        self.assertIn("stdout", published_run["artifacts"])
        self.assertIn("manifest", published_run["artifacts"])
        self.assertNotIn("screenshot", published_run["artifacts"])
        self.assertEqual(published_run["artifacts"]["image_change"], {"changed": False})
        self.assertTrue((output_dir / published_run["artifacts"]["stdout"]).is_file())
        self.assertTrue((output_dir / published_run["artifacts"]["manifest"]).is_file())
        html_text = (output_dir / "index.html").read_text()
        self.assertIn("Gallery &lt;One&gt;", html_text)
        self.assertIn("mac&lt;&gt;/inspect", html_text)

        invalid = output_dir.parent / "zz-invalid"
        invalid.mkdir()
        (invalid / "index.json").write_text("{not json")
        reports = self.mod.desktop_publish_reports(config, limit=1)
        self.assertEqual(len(reports), 1)
        self.assertEqual(reports[0]["label"], "Gallery <One>")
        self.assertEqual(reports[0]["output_dir"], str(output_dir))
        self.assertTrue((output_dir.parent / "latest-report.json").is_file())
        self.assertTrue((output_dir.parent / "reports.jsonl").is_file())


if __name__ == "__main__":
    unittest.main()
