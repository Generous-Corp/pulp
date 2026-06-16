#!/usr/bin/env python3
"""Tests for cleanup command compatibility bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("cleanup_command_bindings.py")


class CleanupCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_cleanup_command_helpers(self):
        expected = (
            *self.mod.CLEANUP_FOOTPRINT_COMMAND_EXPORTS,
            *self.mod.CLEANUP_PLAN_COMMAND_EXPORTS,
            *self.mod.CLEANUP_RUN_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.CLEANUP_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_cleanup_command_helpers_routes_focused_groups_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_cleanup_footprint_command_helpers") as footprint,
            mock.patch.object(self.mod, "install_cleanup_plan_command_helpers") as plan,
            mock.patch.object(self.mod, "install_cleanup_run_command_helpers") as run,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_cleanup_command_helpers(
                bindings,
                (
                    "print_local_ci_state_footprint",
                    "print_local_ci_cleanup_plan",
                    "cmd_cleanup",
                    "custom_cleanup",
                ),
            )

        footprint.assert_called_once_with(bindings, ("print_local_ci_state_footprint",))
        plan.assert_called_once_with(bindings, ("print_local_ci_cleanup_plan",))
        run.assert_called_once_with(bindings, ("cmd_cleanup",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_cleanup",))


if __name__ == "__main__":
    unittest.main()
