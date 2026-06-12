#!/usr/bin/env python3
"""Tests for Windows target constant facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("windows_target_constant_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsTargetConstantBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_constants_delegate_to_windows_target_module(self) -> None:
        windows_target = types.SimpleNamespace(
            WINDOWS_REQUIRED_REMOTE_TOOLS={"git": {"required": True}},
            WINDOWS_OPTIONAL_REMOTE_TOOLS={"gh": {"required": False}},
            WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME="pulp-validate",
        )
        bindings = {"_windows_target": windows_target}

        self.assertIs(self.mod.windows_required_remote_tools(bindings), windows_target.WINDOWS_REQUIRED_REMOTE_TOOLS)
        self.assertIs(self.mod.windows_optional_remote_tools(bindings), windows_target.WINDOWS_OPTIONAL_REMOTE_TOOLS)
        self.assertEqual(self.mod.windows_default_remote_repo_dirname(bindings), "pulp-validate")


if __name__ == "__main__":
    unittest.main()
