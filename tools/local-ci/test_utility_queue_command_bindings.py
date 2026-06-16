#!/usr/bin/env python3
"""Tests for queue utility command compatibility bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("utility_queue_command_bindings.py")


class UtilityQueueCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_queue_command_helpers(self):
        expected = (
            *self.mod.UTILITY_QUEUE_BUMP_COMMAND_EXPORTS,
            *self.mod.UTILITY_QUEUE_CANCEL_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.UTILITY_QUEUE_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_utility_queue_command_helpers_routes_focused_groups_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_utility_queue_bump_command_helpers") as bump,
            mock.patch.object(self.mod, "install_utility_queue_cancel_command_helpers") as cancel,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_utility_queue_command_helpers(
                bindings,
                ("cmd_bump", "cmd_cancel", "custom_queue_command"),
            )

        bump.assert_called_once_with(bindings, ("cmd_bump",))
        cancel.assert_called_once_with(bindings, ("cmd_cancel",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_queue_command",))


if __name__ == "__main__":
    unittest.main()
