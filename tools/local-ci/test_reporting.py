#!/usr/bin/env python3
"""No-network tests for local-ci desktop report helpers."""

from __future__ import annotations

import json
import pathlib
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("reporting.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ReportingTests(unittest.TestCase):
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

    def artifact_root(self, config: dict) -> pathlib.Path:
        path = pathlib.Path(config["desktop_automation"]["artifact_root"])
        path.mkdir(parents=True, exist_ok=True)
        return path

    def publish_root(self, config: dict) -> pathlib.Path:
        path = self.artifact_root(config) / "_published"
        path.mkdir(parents=True, exist_ok=True)
        return path

    def atomic_write(self, path: pathlib.Path, text: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def test_slugify_token_matches_legacy_edges(self) -> None:
        self.assertEqual(self.mod.slugify_token(" UI Preview / Smoke! "), "ui-preview-smoke")
        self.assertEqual(self.mod.slugify_token("!!!"), "run")
        self.assertEqual(len(self.mod.slugify_token("x" * 80, max_len=12)), 12)

    def test_directory_copy_and_clear_helpers_preserve_git_directory(self) -> None:
        src = self.root / "src"
        dest = self.root / "dest"
        (src / "nested").mkdir(parents=True)
        (src / "file.txt").write_text("file")
        (src / "nested" / "child.txt").write_text("child")
        (dest / ".git").mkdir(parents=True)
        (dest / "old.txt").write_text("old")

        self.mod.copy_directory_contents(src, dest)

        self.assertEqual((dest / "file.txt").read_text(), "file")
        self.assertEqual((dest / "nested" / "child.txt").read_text(), "child")

        self.mod.clear_directory_contents(dest)

        self.assertTrue((dest / ".git").is_dir())
        self.assertFalse((dest / "file.txt").exists())
        self.assertFalse((dest / "nested").exists())
        self.assertFalse((dest / "old.txt").exists())

    def test_stage_desktop_publish_report_rejects_empty_manifest_list(self) -> None:
        with self.assertRaisesRegex(ValueError, "requires at least one run manifest"):
            self.mod.stage_desktop_publish_report(
                self.config,
                [],
                create_desktop_publish_bundle_fn=lambda _config: self.publish_root(self.config) / "unused",
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
        output_dir = self.publish_root(self.config) / "20260522-gallery"
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

    def test_manifest_scanning_and_run_rollups_use_latest_passing_proof(self) -> None:
        root = self.artifact_root(self.config)
        old_bundle = root / "mac" / "smoke" / "20260522-old"
        new_bundle = root / "mac" / "smoke" / "20260522-new"
        for bundle in (old_bundle, new_bundle):
            bundle.mkdir(parents=True)
        (old_bundle / "manifest.json").write_text(
            json.dumps(
                {
                    "target": "mac",
                    "action": "smoke",
                    "label": "old",
                    "completed_at": "2026-05-22T12:00:00+00:00",
                    "status": "pass",
                    "source": {"mode": "exact-sha", "sha": "a" * 40},
                }
            )
        )
        (new_bundle / "manifest.json").write_text(
            json.dumps(
                {
                    "target": "mac",
                    "action": "smoke",
                    "label": "new",
                    "completed_at": "2026-05-22T13:00:00+00:00",
                    "status": "error",
                    "source": {"mode": "exact-sha", "sha": "b" * 40},
                }
            )
        )

        manifests = self.mod.desktop_run_manifests(
            self.config,
            target_name="mac",
            action="smoke",
            desktop_artifact_root_fn=self.artifact_root,
        )
        self.assertEqual([manifest["label"] for manifest in manifests], ["new", "old"])
        self.assertEqual(manifests[0]["artifacts"]["bundle_dir"], str(new_bundle))

        def manifests_fn(config: dict, **kwargs):
            return self.mod.desktop_run_manifests(config, desktop_artifact_root_fn=self.artifact_root, **kwargs)

        def summaries_fn(config: dict, **kwargs):
            return self.mod.desktop_proof_summaries(
                config,
                desktop_run_manifests_fn=manifests_fn,
                desktop_run_summary_fn=self.mod.desktop_run_summary,
                **kwargs,
            )

        self.mod.write_desktop_run_rollups(
            self.config,
            target_name="mac",
            desktop_rollup_dir_fn=lambda config, target_name=None: self.mod.desktop_rollup_dir(
                config,
                target_name,
                desktop_artifact_root_fn=self.artifact_root,
            ),
            desktop_run_manifests_fn=manifests_fn,
            desktop_run_summary_fn=self.mod.desktop_run_summary,
            desktop_proof_summaries_fn=summaries_fn,
            atomic_write_text_fn=self.atomic_write,
        )

        latest_run = json.loads((root / "mac" / "latest-run.json").read_text())
        latest_proof = json.loads((root / "mac" / "latest-proof.json").read_text())
        self.assertEqual(latest_run["label"], "new")
        self.assertEqual(latest_run["run_status"], "error")
        self.assertEqual(latest_proof["latest_run"]["label"], "old")

    def test_publish_reports_and_prune_filter_edge_cases(self) -> None:
        old_dir = self.publish_root(self.config) / "old"
        new_dir = self.publish_root(self.config) / "new"
        bad_dir = self.publish_root(self.config) / "bad"
        for path in (old_dir, new_dir, bad_dir):
            path.mkdir(parents=True)
        (old_dir / "index.json").write_text(json.dumps({"generated_at": "2026-01-01T00:00:00Z", "label": "old"}))
        (new_dir / "index.json").write_text(json.dumps({"generated_at": "2026-01-02T00:00:00Z", "label": "new"}))
        (bad_dir / "index.json").write_text("{")

        reports = self.mod.desktop_publish_reports(self.config, desktop_publish_root_fn=self.publish_root)
        self.assertEqual([report["label"] for report in reports], ["new", "old"])

        bundle_a = self.root / "bundle-a"
        bundle_b = self.root / "bundle-b"
        bundle_c = self.root / "bundle-c"
        for bundle in (bundle_a, bundle_b, bundle_c):
            bundle.mkdir()
        manifests = [
            {"completed_at": "2999-01-03T00:00:00+00:00", "artifacts": {"bundle_dir": str(bundle_a)}},
            {"completed_at": "2000-01-01T00:00:00+00:00", "artifacts": {"bundle_dir": str(bundle_b)}},
            {"completed_at": "2000-01-01T00:00:00+00:00", "artifacts": {"bundle_dir": str(bundle_b)}},
            {"completed_at": "invalid", "artifacts": {"bundle_dir": str(bundle_c)}},
        ]
        removed = self.mod.prune_desktop_run_manifests(
            self.config,
            older_than_days=1,
            desktop_run_manifests_fn=lambda _config, **_kwargs: manifests,
        )
        self.assertEqual(removed, [bundle_b])


if __name__ == "__main__":
    unittest.main()
