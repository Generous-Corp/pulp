#!/usr/bin/env python3
"""Tests for validation command construction dependency bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("execution_validation_command_bindings.py")


class ExecutionValidationCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_validation_command_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.EXECUTION_LOCAL_COMMAND_EXPORTS,
            *self.mod.EXECUTION_POSIX_COMMAND_EXPORTS,
            *self.mod.EXECUTION_WINDOWS_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.EXECUTION_VALIDATION_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_validation_command_installer_routes_focused_groups(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_execution_local_command_helpers") as install_local_cmd,
            mock.patch.object(self.mod, "install_execution_posix_command_helpers") as install_posix_cmd,
            mock.patch.object(self.mod, "install_execution_windows_command_helpers") as install_windows_cmd,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_execution_validation_command_helpers(
                bindings,
                ("local_validation_command", "posix_ssh_validation_command", "windows_validation_script", "unknown_helper"),
            )

        install_local_cmd.assert_called_once_with(bindings, ("local_validation_command",))
        install_posix_cmd.assert_called_once_with(bindings, ("posix_ssh_validation_command",))
        install_windows_cmd.assert_called_once_with(bindings, ("windows_validation_script",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
