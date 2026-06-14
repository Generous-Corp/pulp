#!/usr/bin/env python3
"""Tests for Windows validation script dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("execution_windows_command_bindings.py")


class ExecutionWindowsCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_windows_command_exports_match_facade_helpers(self):
        expected = ("windows_validation_script",)

        self.assertEqual(self.mod.EXECUTION_WINDOWS_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_windows_validation_script_delegates_to_execution_module(self):
        captured = {}

        def windows_validation_script(*args, **kwargs):
            captured["windows"] = (args, kwargs)
            return "script", "full"

        ps_literal = object()
        execution = types.SimpleNamespace(windows_validation_script=windows_validation_script)
        bindings = {"_execution": execution, "ps_literal": ps_literal}

        self.assertEqual(
            self.mod.windows_validation_script(
                bindings,
                "windows",
                "host",
                r"C:\Repo",
                {"id": "job"},
                bundle_name="bundle",
                bundle_ref="ref",
                exclude_tests="slow",
                cmake_generator="Ninja",
                resolved_platform="ARM64",
                resolved_generator_instance=r"C:\VS",
            ),
            ("script", "full"),
        )
        self.assertEqual(captured["windows"][0], ("windows", "host", r"C:\Repo", {"id": "job"}))
        self.assertEqual(captured["windows"][1]["cmake_generator"], "Ninja")
        self.assertEqual(captured["windows"][1]["resolved_platform"], "ARM64")
        self.assertEqual(captured["windows"][1]["resolved_generator_instance"], r"C:\VS")
        self.assertIs(captured["windows"][1]["ps_literal_fn"], ps_literal)

if __name__ == "__main__":
    unittest.main()
