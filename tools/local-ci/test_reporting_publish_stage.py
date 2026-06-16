#!/usr/bin/env python3
"""Tests for desktop publish report staging."""

from __future__ import annotations

import json
import pathlib
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("reporting_publish_stage.py", add_module_dir=True)


class ReportingPublishStageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)
        self.config = {
            "desktop_automation": {
                "artifact_root": str(self.root / "desktop-artifacts"),
                "publish_mode": "local",
                "publish_branch": "dev-artifacts",
            }
        }

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def publish_root(self) -> pathlib.Path:
        path = self.root / "desktop-artifacts" / "_published"
        path.mkdir(parents=True, exist_ok=True)
        return path

    def atomic_write(self, path: pathlib.Path, text: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def test_slugify_token_matches_legacy_edges(self) -> None:
        self.assertEqual(self.mod.slugify_token(" UI Preview / Smoke! "), "ui-preview-smoke")
        self.assertEqual(self.mod.slugify_token("!!!"), "run")
        self.assertEqual(len(self.mod.slugify_token("x" * 80, max_len=12)), 12)

    def test_stage_desktop_publish_report_rejects_empty_manifest_list(self) -> None:
        with self.assertRaisesRegex(ValueError, "requires at least one run manifest"):
            self.mod.stage_desktop_publish_report(
                self.config,
                [],
                create_desktop_publish_bundle_fn=lambda _config: self.publish_root() / "unused",
                now_iso_fn=lambda: "2026-05-22T12:01:00+00:00",
                atomic_write_text_fn=self.atomic_write,
                write_desktop_publish_rollups_fn=lambda _config: None,
                publish_report_to_branch_fn=lambda _config, _report: {},
            )

    def test_stage_desktop_publish_report_copies_artifacts_and_writes_index(self) -> None:
        bundle = self.root / "bundle"
        bundle.mkdir()
        stdout_log = bundle / "stdout.log"
        stdout_log.write_text("hello\n")
        (bundle / "manifest.json").write_text('{"label":"bundle-copy"}\n')
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
        output_dir = self.publish_root() / "20260522-gallery"
        rollups: list[dict] = []

        report = self.mod.stage_desktop_publish_report(
            self.config,
            [manifest],
            output_dir=output_dir,
            label="Gallery <One>",
            create_desktop_publish_bundle_fn=lambda _config: output_dir,
            now_iso_fn=lambda: "2026-05-22T12:01:00+00:00",
            atomic_write_text_fn=self.atomic_write,
            write_desktop_publish_rollups_fn=lambda config: rollups.append(config),
            publish_report_to_branch_fn=lambda _config, _report: {},
        )

        self.assertEqual(report["label"], "Gallery <One>")
        self.assertEqual(report["run_count"], 1)
        self.assertEqual(len(rollups), 1)
        payload = json.loads((output_dir / "index.json").read_text())
        published_run = payload["runs"][0]
        self.assertEqual(payload["publish_mode"], "local")
        self.assertEqual(published_run["target"], "mac<>")
        self.assertEqual(published_run["interaction_mode"], "dom")
        self.assertIn("stdout", published_run["artifacts"])
        self.assertIn("manifest", published_run["artifacts"])
        self.assertNotIn("screenshot", published_run["artifacts"])
        self.assertEqual(published_run["artifacts"]["image_change"], {"changed": False})
        html_text = (output_dir / "index.html").read_text()
        self.assertIn("Gallery &lt;One&gt;", html_text)
        self.assertIn("mac&lt;&gt;/inspect", html_text)

    def test_stage_writes_review_package_and_enriches_video_proof_runs(self) -> None:
        bundle = self.root / "vbundle"
        bundle.mkdir()
        (bundle / "manifest.json").write_text('{"label":"video"}\n')
        manifest = {
            "target": "mac",
            "action": "click",
            "label": "video proof",
            "completed_at": "2026-06-16T12:00:00+00:00",
            "artifacts": {"bundle_dir": str(bundle)},
            "interaction": {"mode": "desktop-event"},
            "video_proof_notes": ["toggle flips"],
            "video_proof_composition": {
                "template": "component-zoom",
                "focus": {"label": "Bypass"},
            },
        }
        output_dir = self.publish_root() / "20260616-proof"

        report = self.mod.stage_desktop_publish_report(
            self.config,
            [manifest],
            output_dir=output_dir,
            label="Proof",
            create_desktop_publish_bundle_fn=lambda _config: output_dir,
            now_iso_fn=lambda: "2026-06-16T12:01:00+00:00",
            atomic_write_text_fn=self.atomic_write,
            write_desktop_publish_rollups_fn=lambda _config: None,
            publish_report_to_branch_fn=lambda _config, _report: {},
        )

        review_package_path = output_dir / "review-package.json"
        review_markdown_path = output_dir / "review.md"
        self.assertTrue(review_package_path.exists(), "publish must write review-package.json")
        self.assertTrue(review_markdown_path.exists(), "publish must write review.md")
        self.assertEqual(report["review_package"], str(review_package_path))
        self.assertEqual(report["review_markdown"], str(review_markdown_path))
        package = json.loads(review_package_path.read_text())
        self.assertIsInstance(package, dict)
        run = json.loads((output_dir / "index.json").read_text())["runs"][0]
        self.assertEqual(run["video_proof_composition"]["focus"]["label"], "Bypass")
        self.assertEqual(run["video_proof_notes"], ["toggle flips"])


if __name__ == "__main__":
    unittest.main()
