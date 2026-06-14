#!/usr/bin/env python3
"""Tests for desktop management command facade bindings."""

from __future__ import annotations

import unittest
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_management_command_bindings.py")


class DesktopManagementCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_STATUS_COMMAND_EXPORTS,
            *self.mod.DESKTOP_CONFIG_COMMAND_EXPORTS,
            *self.mod.DESKTOP_REPORT_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_MANAGEMENT_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_management_command_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_status_command_helpers") as install_status,
            mock.patch.object(self.mod, "install_desktop_config_command_helpers") as install_config,
            mock.patch.object(self.mod, "install_desktop_report_command_helpers") as install_report,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_management_command_helpers(
                bindings,
                ("cmd_desktop_status", "cmd_desktop_config_show", "cmd_desktop_recent", "custom_management_command"),
            )

        install_status.assert_called_once_with(bindings, ("cmd_desktop_status",))
        install_config.assert_called_once_with(bindings, ("cmd_desktop_config_show",))
        install_report.assert_called_once_with(bindings, ("cmd_desktop_recent",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_management_command",))


if __name__ == "__main__":
    unittest.main()
