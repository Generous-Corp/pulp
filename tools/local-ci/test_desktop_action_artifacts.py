#!/usr/bin/env python3
"""No-network tests for desktop action artifact path helpers."""

from __future__ import annotations

import pathlib
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_action_artifacts.py")


class DesktopActionArtifactsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_desktop_action_artifact_paths_use_stable_bundle_layout(self) -> None:
        bundle_dir = pathlib.Path("/tmp/local-ci-bundle")
        paths = self.mod.desktop_action_artifact_paths(bundle_dir)

        self.assertEqual(paths["screenshot"], bundle_dir / "screenshots" / "window.png")
        self.assertEqual(paths["before_screenshot"], bundle_dir / "screenshots" / "before.png")
        self.assertEqual(paths["diff_screenshot"], bundle_dir / "screenshots" / "diff.png")
        self.assertEqual(paths["ui_snapshot"], bundle_dir / "ui-tree.json")
        self.assertEqual(paths["stdout"], bundle_dir / "stdout.log")
        self.assertEqual(paths["stderr"], bundle_dir / "stderr.log")

    def test_desktop_action_artifact_paths_expand_explicit_output_path(self) -> None:
        paths = self.mod.desktop_action_artifact_paths(pathlib.Path("/tmp/local-ci-bundle"), "~/Desktop/window.png")

        self.assertEqual(paths["screenshot"], pathlib.Path.home() / "Desktop" / "window.png")


if __name__ == "__main__":
    unittest.main()
