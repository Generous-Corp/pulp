#!/usr/bin/env python3
"""Tests for validation runner facade bindings."""

from __future__ import annotations

from pathlib import Path
import unittest

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
        calls = []

        def local_install(bindings, names):
            calls.append(("local", names))

        def ssh_install(bindings, names):
            calls.append(("ssh", names))

        def windows_install(bindings, names):
            calls.append(("windows", names))

        def fallback_install(bindings, globals_obj, names):
            calls.append(("local_fallback", names))

        self.mod.install_execution_runner_local_helpers = local_install
        self.mod.install_execution_runner_ssh_helpers = ssh_install
        self.mod.install_execution_runner_windows_helpers = windows_install
        self.mod.install_local_helpers = fallback_install

        self.mod.install_execution_runner_helpers(
            {},
            (
                "run_local_validation",
                "run_posix_ssh_validation",
                "run_windows_ssh_validation",
                "windows_validation_script",
                "custom_execution_runner_export",
            ),
        )

        self.assertEqual(
            calls,
            [
                ("local", ("run_local_validation",)),
                ("ssh", ("run_posix_ssh_validation",)),
                ("windows", ("run_windows_ssh_validation", "windows_validation_script")),
                ("local_fallback", ("custom_execution_runner_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
