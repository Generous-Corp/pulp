#!/usr/bin/env python3
"""Tests for Windows SSH validation runner facade bindings."""

from __future__ import annotations

from pathlib import Path
import unittest
from unittest import mock

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
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_execution_runner_windows_run_helpers") as run,
            mock.patch.object(self.mod, "install_execution_runner_windows_script_helpers") as script,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_execution_runner_windows_helpers(
                bindings,
                (
                    "run_windows_ssh_validation",
                    "windows_validation_script",
                    "custom_windows_runner_export",
                ),
            )

        run.assert_called_once_with(bindings, ("run_windows_ssh_validation",))
        script.assert_called_once_with(bindings, ("windows_validation_script",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_windows_runner_export",))


if __name__ == "__main__":
    unittest.main()
