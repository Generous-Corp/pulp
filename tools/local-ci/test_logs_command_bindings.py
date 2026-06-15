#!/usr/bin/env python3
"""Tests for logs command compatibility bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("logs_command_bindings.py")


class LogsCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_logs_command_helpers(self):
        expected = (
            *self.mod.LOGS_RESOLUTION_COMMAND_EXPORTS,
            *self.mod.LOGS_RUN_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.LOGS_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_logs_command_helpers_routes_focused_groups_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_logs_resolution_command_helpers") as resolution,
            mock.patch.object(self.mod, "install_logs_run_command_helpers") as run,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_logs_command_helpers(
                bindings,
                ("resolve_job_for_logs", "cmd_logs", "custom_logs"),
            )

        resolution.assert_called_once_with(bindings, ("resolve_job_for_logs",))
        run.assert_called_once_with(bindings, ("cmd_logs",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_logs",))


if __name__ == "__main__":
    unittest.main()
