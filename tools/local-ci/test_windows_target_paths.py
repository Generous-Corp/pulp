#!/usr/bin/env python3
"""No-network tests for Windows target path helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_target_paths.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsTargetPathsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_path_repo_safety_and_config_helpers(self) -> None:
        self.assertEqual(self.mod.windows_path_join("", r"C:\Root\\", r"\child", ""), r"C:\Root\child")
        self.assertEqual(self.mod.windows_default_repo_checkout_path(None), "pulp-validate")
        self.assertEqual(
            self.mod.windows_default_repo_checkout_path(r"C:\Users\dev"),
            r"C:\Users\dev\pulp-validate",
        )
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(None))
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(r"C:\\"))
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(r"C:\Users\dev", r"C:\Users\dev"))
        self.assertFalse(self.mod.windows_repo_path_is_unsafe(r"C:\Users\dev\pulp", r"C:\Users\dev"))

        config: dict = {}
        self.mod.update_target_repo_path(config, "windows", r"C:\Pulp")
        self.assertEqual(config["targets"]["windows"]["repo_path"], r"C:\Pulp")
        self.assertEqual(config["desktop_automation"]["targets"]["windows"]["repo_path"], r"C:\Pulp")

    def test_repo_checkout_ready_requires_safe_complete_checkout(self) -> None:
        self.assertFalse(self.mod.windows_repo_checkout_ready(None))
        self.assertFalse(
            self.mod.windows_repo_checkout_ready(
                {"git_dir_exists": True, "head_exists": True, "setup_exists": True, "repo_path_unsafe": True}
            )
        )
        self.assertTrue(
            self.mod.windows_repo_checkout_ready(
                {"git_dir_exists": True, "head_exists": True, "setup_exists": True, "repo_path_unsafe": False}
            )
        )


if __name__ == "__main__":
    unittest.main()
