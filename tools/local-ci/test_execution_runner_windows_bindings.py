#!/usr/bin/env python3
"""Tests for Windows SSH validation runner facade bindings."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("execution_runner_windows_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionRunnerWindowsBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_windows_runner_exports_match_wrappers(self) -> None:
        expected = (
            *self.mod.EXECUTION_RUNNER_WINDOWS_RUN_EXPORTS,
            *self.mod.EXECUTION_RUNNER_WINDOWS_SCRIPT_EXPORTS,
        )

        self.assertEqual(self.mod.EXECUTION_RUNNER_WINDOWS_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_execution_runner_windows_helpers_routes_focused_and_unknown_exports(self) -> None:
        calls = []

        def run_install(bindings, names):
            calls.append(("run", names))

        def script_install(bindings, names):
            calls.append(("script", names))

        def fallback_install(bindings, globals_obj, names):
            calls.append(("local_fallback", names))

        self.mod.install_execution_runner_windows_run_helpers = run_install
        self.mod.install_execution_runner_windows_script_helpers = script_install
        self.mod.install_local_helpers = fallback_install

        self.mod.install_execution_runner_windows_helpers(
            {},
            (
                "run_windows_ssh_validation",
                "windows_validation_script",
                "custom_windows_runner_export",
            ),
        )

        self.assertEqual(
            calls,
            [
                ("run", ("run_windows_ssh_validation",)),
                ("script", ("windows_validation_script",)),
                ("local_fallback", ("custom_windows_runner_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
