#!/usr/bin/env python3
"""Tests for PR-oriented local-CI command dependency bindings."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci_pr_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiPrCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_pr_command_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.LOCAL_CI_PR_SHIP_COMMAND_EXPORTS,
            *self.mod.LOCAL_CI_PR_CHECK_COMMAND_EXPORTS,
            *self.mod.LOCAL_CI_PR_LIST_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.LOCAL_CI_PR_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_pr_command_helpers_routes_selected_groups_and_unknown_exports(self):
        bindings = {}
        names = ("cmd_ship", "cmd_list", "custom")

        with (
            mock.patch.object(self.mod, "install_local_ci_pr_ship_command_helpers") as ship,
            mock.patch.object(self.mod, "install_local_ci_pr_check_command_helpers") as check,
            mock.patch.object(self.mod, "install_local_ci_pr_list_command_helpers") as list_,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_local_ci_pr_command_helpers(bindings, names)

        ship.assert_called_once_with(bindings, ("cmd_ship",))
        check.assert_called_once_with(bindings, ())
        list_.assert_called_once_with(bindings, ("cmd_list",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
