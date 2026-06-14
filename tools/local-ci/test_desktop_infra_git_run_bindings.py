#!/usr/bin/env python3
"""Tests for desktop git command execution bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("desktop_infra_git_run_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopInfraGitRunBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_run_exports_match_wrappers(self) -> None:
        expected = ("run_git",)

        self.assertEqual(self.mod.DESKTOP_INFRA_GIT_RUN_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_run_git_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def run_git(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return types.SimpleNamespace(returncode=0)

        bindings = {
            "_git_helpers": types.SimpleNamespace(run_git=run_git),
        }
        repo_root = Path("/repo")
        deps = {"run_fn": object()}

        with mock.patch.object(self.mod, "run_git_dependencies", return_value=deps):
            self.assertEqual(self.mod.run_git(bindings, ["status"], cwd=repo_root, check=False).returncode, 0)
        self.assertEqual(captured["args"], (["status"],))
        self.assertEqual(captured["kwargs"]["cwd"], repo_root)
        self.assertEqual(captured["kwargs"]["check"], False)
        self.assertIs(captured["kwargs"]["run_fn"], deps["run_fn"])

if __name__ == "__main__":
    unittest.main()
