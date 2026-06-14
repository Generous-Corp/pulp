#!/usr/bin/env python3
"""Tests for desktop action runner command compatibility bindings."""

from __future__ import annotations

import unittest
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_action_run_command_bindings.py")


class DesktopActionRunCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_run_command_exports_match_wrappers(self) -> None:
        expected = (
            *self.mod.DESKTOP_ACTION_SMOKE_COMMAND_EXPORTS,
            *self.mod.DESKTOP_ACTION_CLICK_COMMAND_EXPORTS,
            *self.mod.DESKTOP_ACTION_INSPECT_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_ACTION_RUN_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_action_run_command_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_action_smoke_command_helpers") as install_smoke,
            mock.patch.object(self.mod, "install_desktop_action_click_command_helpers") as install_click,
            mock.patch.object(self.mod, "install_desktop_action_inspect_command_helpers") as install_inspect,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_action_run_command_helpers(
                bindings,
                ("cmd_desktop_smoke", "cmd_desktop_click", "cmd_desktop_inspect", "custom_action"),
            )

        install_smoke.assert_called_once_with(bindings, ("cmd_desktop_smoke",))
        install_click.assert_called_once_with(bindings, ("cmd_desktop_click",))
        install_inspect.assert_called_once_with(bindings, ("cmd_desktop_inspect",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_action",))

if __name__ == "__main__":
    unittest.main()
