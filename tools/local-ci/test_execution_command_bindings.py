#!/usr/bin/env python3
"""Tests for validation command facade dependency bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("execution_command_bindings.py")


class ExecutionCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_facade_reexports_command_state_and_validation_command_helpers(self):
        expected_exports = (
            "remote_commit_error",
            "prepared_state_root",
            "should_reuse_prepared_state",
            "local_validation_command",
            "posix_ssh_validation_command",
            "windows_validation_script",
        )

        self.assertEqual(self.mod.EXECUTION_COMMAND_EXPORTS, expected_exports)
        for name in expected_exports:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_command_installer_routes_selected_groups_and_fallback(self):
        bindings = {"_execution": object()}

        with (
            mock.patch.object(self.mod, "install_execution_command_state_helpers") as command_state,
            mock.patch.object(self.mod, "install_execution_validation_command_helpers") as validation_command,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_execution_command_helpers(
                bindings,
                ("remote_commit_error", "local_validation_command", "unknown_helper"),
            )

        command_state.assert_called_once_with(bindings, ("remote_commit_error",))
        validation_command.assert_called_once_with(bindings, ("local_validation_command",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
