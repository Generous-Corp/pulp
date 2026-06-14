#!/usr/bin/env python3
"""No-network tests for Windows target probe formatting helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_target_probe_format.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsTargetProbeFormatTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_formatting_and_readiness_helpers(self) -> None:
        self.assertEqual(self.mod.windows_desktop_session_user(None), "")
        self.assertEqual(self.mod.windows_desktop_session_user({"logged_on_user": " dev "}), "dev")
        self.assertEqual(self.mod.windows_desktop_session_state({"session_state": " Active "}), "Active")
        self.assertEqual(
            self.mod.windows_tooling_detail({"git_found": True, "git_path": r"C:\Git\git.exe"}, "git"),
            r"C:\Git\git.exe",
        )
        self.assertEqual(
            self.mod.windows_tooling_detail({"git_found": True, "git_version": "git 2.49", "git_path": "git.exe"}, "git"),
            "git 2.49 (git.exe)",
        )
        self.assertEqual(self.mod.windows_tooling_detail({}, "git", missing_hint="install git"), "install git")
        self.assertTrue(self.mod.windows_remote_tooling_ready({"git_found": True}))
        self.assertFalse(self.mod.windows_remote_tooling_ready({}))
        self.assertIn(
            "not a git checkout",
            self.mod.windows_repo_checkout_detail({"repo_path": r"C:\Pulp", "repo_exists": True}),
        )
        self.assertIn(
            "empty git repo",
            self.mod.windows_repo_checkout_detail({"repo_path": r"C:\Pulp", "git_dir_exists": True}),
        )
        self.assertIn(
            "checkout incomplete",
            self.mod.windows_repo_checkout_detail(
                {"repo_path": r"C:\Pulp", "git_dir_exists": True, "head_exists": True}
            ),
        )
        self.assertEqual(
            self.mod.windows_repo_checkout_detail({"repo_path": r"C:\Pulp", "origin_url": "https://example/repo.git"}),
            r"C:\Pulp (https://example/repo.git)",
        )


if __name__ == "__main__":
    unittest.main()
