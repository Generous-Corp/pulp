#!/usr/bin/env python3
"""Tests for validation logging/progress dependency bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("execution_logging_bindings.py")


class ExecutionLoggingBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_logging_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.EXECUTION_LOGGING_TIMING_EXPORTS,
            *self.mod.EXECUTION_PROGRESS_MARKER_EXPORTS,
            *self.mod.EXECUTION_LOGGED_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.EXECUTION_LOGGING_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_logging_installer_routes_selected_groups_and_fallback(self):
        bindings = {"_execution": object()}

        with (
            mock.patch.object(self.mod, "install_execution_logging_timing_helpers") as timing,
            mock.patch.object(self.mod, "install_execution_progress_marker_helpers") as progress,
            mock.patch.object(self.mod, "install_execution_logged_command_helpers") as command,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_execution_logging_helpers(
                bindings,
                ("heartbeat_interval_secs", "parse_progress_marker", "run_logged_command", "unknown_helper"),
            )

        timing.assert_called_once_with(bindings, ("heartbeat_interval_secs",))
        progress.assert_called_once_with(bindings, ("parse_progress_marker",))
        command.assert_called_once_with(bindings, ("run_logged_command",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
