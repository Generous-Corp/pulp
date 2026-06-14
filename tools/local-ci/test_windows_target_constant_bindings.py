#!/usr/bin/env python3
"""Tests for Windows target constant facade bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("windows_target_constant_bindings.py")


class WindowsTargetConstantBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_are_named_constant_helpers(self) -> None:
        expected = (
            "windows_required_remote_tools",
            "windows_optional_remote_tools",
            "windows_default_remote_repo_dirname",
        )

        self.assertEqual(self.mod.WINDOWS_TARGET_CONSTANT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

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
