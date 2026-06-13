#!/usr/bin/env python3
"""Tests for Windows validation script facade bindings."""

from __future__ import annotations

import types
import unittest
from pathlib import Path

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("execution_runner_windows_script_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionRunnerWindowsScriptBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_windows_script_exports_match_wrappers(self) -> None:
        expected = ("windows_validation_script",)

        self.assertEqual(self.mod.EXECUTION_RUNNER_WINDOWS_SCRIPT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def _bindings(self, script_runner):
        execution = types.SimpleNamespace(windows_validation_script=script_runner)
        return {"_execution": execution, "ps_literal": object()}

    def test_windows_validation_script_binds_literal_escape(self) -> None:
        captured = {}

        def script_runner(*args, **kwargs):
            captured["script"] = (args, kwargs)
            return "script", "full"

        bindings = self._bindings(script_runner)

        script = self.mod.windows_validation_script(
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
        )

        self.assertEqual(script, ("script", "full"))
        self.assertIs(captured["script"][1]["ps_literal_fn"], bindings["ps_literal"])

    def test_install_execution_runner_windows_script_helpers_wires_named_exports(self) -> None:
        captured = {}

        def script_runner(*args, **kwargs):
            captured["script"] = (args, kwargs)
            return "script", "full"

        bindings = self._bindings(script_runner)
        self.mod.install_execution_runner_windows_script_helpers(bindings)

        self.assertEqual(
            bindings["windows_validation_script"](
                "windows",
                "host",
                r"C:\Repo",
                {"id": "job"},
                bundle_name="bundle",
                bundle_ref="ref",
                exclude_tests="",
                cmake_generator="Ninja",
                resolved_platform="ARM64",
                resolved_generator_instance="",
            ),
            ("script", "full"),
        )
        self.assertIs(captured["script"][1]["ps_literal_fn"], bindings["ps_literal"])

    def test_install_execution_runner_windows_script_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_execution_runner_windows_script_helper = lambda _bindings: "future"

        self.mod.install_execution_runner_windows_script_helpers(
            bindings,
            ("future_execution_runner_windows_script_helper",),
        )

        self.assertEqual(bindings["future_execution_runner_windows_script_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
