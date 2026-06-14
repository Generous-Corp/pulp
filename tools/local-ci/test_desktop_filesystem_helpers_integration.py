#!/usr/bin/env python3
"""Facade-level desktop bundle and filesystem helper integration tests."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(
        MODULE_PATH,
        module_name="pulp_local_ci_desktop_filesystem_helpers_integration",
        add_module_dir=True,
    )


class DesktopFilesystemHelpersIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_desktop_bundle_and_prune_helpers_cover_edge_filters(self) -> None:
        config = {"desktop_automation": {"artifact_root": str(self.root / "artifacts")}}
        run_bundle = self.mod.create_desktop_run_bundle(config, "mac", "smoke")
        publish_bundle = self.mod.create_desktop_publish_bundle(config)
        self.assertTrue((run_bundle / "screenshots").is_dir())
        self.assertEqual(run_bundle.parents[1].name, "mac")
        self.assertTrue((publish_bundle / "assets").is_dir())
        self.assertEqual(publish_bundle.parent, self.root / "artifacts" / "_published")

        bundle_a = self.root / "bundle-a"
        bundle_b = self.root / "bundle-b"
        bundle_a.mkdir()
        bundle_b.mkdir()
        manifests = [
            {"completed_at": "bad-date", "artifacts": {"bundle_dir": str(bundle_a)}},
            {"completed_at": "2000-01-01T00:00:00Z", "artifacts": {"bundle_dir": str(bundle_a)}},
            {"started_at": "2000-01-02T00:00:00Z", "artifacts": {"bundle_dir": str(bundle_b)}},
            {"completed_at": "2999-01-01T00:00:00Z", "artifacts": {"bundle_dir": str(self.root / "missing")}},
        ]
        with mock.patch.object(self.mod, "desktop_run_manifests", return_value=manifests):
            self.assertEqual(self.mod.prune_desktop_run_manifests(config, older_than_days=1), [bundle_a, bundle_b])
            self.assertEqual(self.mod.prune_desktop_run_manifests(config, keep_last=2), [bundle_b])

    def test_filesystem_and_git_wrappers_cover_fallbacks(self) -> None:
        src = self.root / "src"
        dest = self.root / "dest"
        src.mkdir()
        (src / "nested").mkdir()
        (src / "nested" / "file.txt").write_text("nested")
        (src / "top.txt").write_text("top")
        self.mod._copy_directory_contents(src, dest)
        self.assertEqual((dest / "nested" / "file.txt").read_text(), "nested")
        self.assertEqual((dest / "top.txt").read_text(), "top")

        keep_git = dest / ".git"
        keep_git.mkdir()
        self.mod._clear_directory_contents(dest)
        self.assertTrue(keep_git.exists())
        self.assertFalse((dest / "nested").exists())
        self.assertFalse((dest / "top.txt").exists())

        ok = subprocess.CompletedProcess(["git"], 0, stdout="ok", stderr="")
        fail = subprocess.CompletedProcess(["git"], 2, stdout="", stderr="bad ref")
        with mock.patch.object(self.mod.subprocess, "run", return_value=ok) as run:
            self.assertIs(self.mod._run_git(["status"], cwd=self.root), ok)
            self.assertEqual(run.call_args.args[0], ["git", "status"])
        with mock.patch.object(self.mod.subprocess, "run", return_value=fail):
            self.assertIs(self.mod._run_git(["status"], cwd=self.root, check=False), fail)
            with self.assertRaisesRegex(RuntimeError, "bad ref"):
                self.mod._run_git(["status"], cwd=self.root)

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 1, stdout="", stderr="no remote"),
        ):
            self.assertIsNone(self.mod.git_origin_http_url(self.root))
            self.assertIsNone(self.mod.git_origin_clone_url(self.root))


if __name__ == "__main__":
    unittest.main()
