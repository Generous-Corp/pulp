#!/usr/bin/env python3
"""Tests for utility command facade bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("utility_command_bindings.py")


class UtilityCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_compose_focused_command_groups(self):
        self.assertEqual(
            self.mod.UTILITY_COMMAND_EXPORTS,
            (
                *self.mod.CLEANUP_COMMAND_EXPORTS,
                *self.mod.UTILITY_QUEUE_COMMAND_EXPORTS,
                *self.mod.LOGS_COMMAND_EXPORTS,
                *self.mod.EVIDENCE_COMMAND_EXPORTS,
            ),
        )
        self.assertEqual(len(self.mod.UTILITY_COMMAND_EXPORTS), len(set(self.mod.UTILITY_COMMAND_EXPORTS)))

    def test_install_utility_command_helpers_routes_focused_groups_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_cleanup_command_helpers") as cleanup,
            mock.patch.object(self.mod, "install_utility_queue_command_helpers") as queue,
            mock.patch.object(self.mod, "install_logs_command_helpers") as logs,
            mock.patch.object(self.mod, "install_evidence_command_helpers") as evidence,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_utility_command_helpers(
                bindings,
                names=("cmd_cleanup", "cmd_bump", "cmd_logs", "cmd_evidence", "external"),
            )

        cleanup.assert_called_once_with(bindings, ("cmd_cleanup",))
        queue.assert_called_once_with(bindings, ("cmd_bump",))
        logs.assert_called_once_with(bindings, ("cmd_logs",))
        evidence.assert_called_once_with(bindings, ("cmd_evidence",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("external",))


if __name__ == "__main__":
    unittest.main()
