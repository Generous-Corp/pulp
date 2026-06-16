#!/usr/bin/env python3
"""Tests for desktop command facade composition."""

import unittest
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_command_bindings.py")


class DesktopCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_command_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_SETUP_COMMAND_EXPORTS,
            *self.mod.DESKTOP_MANAGEMENT_COMMAND_EXPORTS,
            *self.mod.DESKTOP_ACTION_COMMAND_EXPORTS,
            *self.mod.DESKTOP_REVIEW_COMMAND_EXPORTS,
            *self.mod.DESKTOP_VIDEO_COMPOSE_COMMAND_EXPORTS,
            *self.mod.DESKTOP_VIDEO_INFO_COMMAND_EXPORTS,
            *self.mod.DESKTOP_VIDEO_ACTION_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_command_helpers_routes_each_group(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_setup_command_helpers") as install_setup,
            mock.patch.object(self.mod, "install_desktop_management_command_helpers") as install_management,
            mock.patch.object(self.mod, "install_desktop_action_command_helpers") as install_action,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_command_helpers(
                bindings,
                ("cmd_desktop_install", "cmd_desktop_status", "cmd_desktop_smoke", "custom_desktop_command"),
            )

        install_setup.assert_called_once_with(bindings, ("cmd_desktop_install",))
        install_management.assert_called_once_with(bindings, ("cmd_desktop_status",))
        install_action.assert_called_once_with(bindings, ("cmd_desktop_smoke",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_desktop_command",))


if __name__ == "__main__":
    unittest.main()
