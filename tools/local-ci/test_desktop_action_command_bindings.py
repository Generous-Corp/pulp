#!/usr/bin/env python3
"""Tests for desktop action command facade bindings."""

from __future__ import annotations

import unittest
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_action_command_bindings.py")


class DesktopActionCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_ACTION_SELECTOR_EXPORTS,
            *self.mod.DESKTOP_ACTION_RUN_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_ACTION_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_action_command_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_action_selector_helpers") as install_selector,
            mock.patch.object(self.mod, "install_desktop_action_run_command_helpers") as install_run,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_action_command_helpers(
                bindings,
                ("windows_requires_pulp_app_selectors", "cmd_desktop_smoke", "custom_action_command"),
            )

        install_selector.assert_called_once_with(bindings, ("windows_requires_pulp_app_selectors",))
        install_run.assert_called_once_with(bindings, ("cmd_desktop_smoke",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_action_command",))


if __name__ == "__main__":
    unittest.main()
