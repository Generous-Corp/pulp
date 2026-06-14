#!/usr/bin/env python3
"""Tests for validation runner facade bindings."""

from __future__ import annotations

from pathlib import Path
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("execution_runner_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionRunnerBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_runner_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.EXECUTION_RUNNER_LOCAL_EXPORTS,
            *self.mod.EXECUTION_RUNNER_SSH_EXPORTS,
            *self.mod.EXECUTION_RUNNER_WINDOWS_EXPORTS,
        )

        self.assertEqual(self.mod.EXECUTION_RUNNER_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_execution_runner_helpers_routes_each_group_and_unknown_exports(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_execution_runner_local_helpers") as local,
            mock.patch.object(self.mod, "install_execution_runner_ssh_helpers") as ssh,
            mock.patch.object(self.mod, "install_execution_runner_windows_helpers") as windows,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_execution_runner_helpers(
                bindings,
                (
                    "run_local_validation",
                    "run_posix_ssh_validation",
                    "run_windows_ssh_validation",
                    "windows_validation_script",
                    "custom_execution_runner_export",
                ),
            )

        local.assert_called_once_with(bindings, ("run_local_validation",))
        ssh.assert_called_once_with(bindings, ("run_posix_ssh_validation",))
        windows.assert_called_once_with(bindings, ("run_windows_ssh_validation", "windows_validation_script"))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_execution_runner_export",))


if __name__ == "__main__":
    unittest.main()
