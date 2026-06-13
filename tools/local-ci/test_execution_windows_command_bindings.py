#!/usr/bin/env python3
"""Tests for Windows validation script dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("execution_windows_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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

    def test_windows_command_installer_wires_selected_exports(self):
        def windows_validation_script(*args, **kwargs):
            return "script", "full"

        bindings = {
            "_execution": types.SimpleNamespace(windows_validation_script=windows_validation_script),
            "ps_literal": object(),
        }

        self.mod.install_execution_windows_command_helpers(bindings, ("windows_validation_script",))

        self.assertEqual(
            bindings["windows_validation_script"](
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

    def test_install_execution_windows_command_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_execution_windows_command_helper = lambda _bindings: "future"

        self.mod.install_execution_windows_command_helpers(bindings, ("future_execution_windows_command_helper",))

        self.assertEqual(bindings["future_execution_windows_command_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
