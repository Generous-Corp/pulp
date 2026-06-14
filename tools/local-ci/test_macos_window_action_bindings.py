#!/usr/bin/env python3
"""Tests for macOS window action facade bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("macos_window_action_bindings.py")


class MacosWindowActionBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_action_exports_match_focused_groups(self) -> None:
        expected = (
            *self.mod.MACOS_WINDOW_ACTIVATION_EXPORTS,
            *self.mod.MACOS_WINDOW_CLICK_EXPORTS,
            *self.mod.MACOS_WINDOW_PROCESS_EXPORTS,
        )
        self.assertEqual(self.mod.MACOS_WINDOW_ACTION_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_action_installer_routes_selected_groups(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_macos_window_activation_helpers") as install_activation,
            mock.patch.object(self.mod, "install_macos_window_click_helpers") as install_click,
            mock.patch.object(self.mod, "install_macos_window_process_helpers") as install_process,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_macos_window_action_helpers(
                bindings,
                ("activate_macos_bundle_id", "dispatch_macos_click", "quit_macos_bundle_id", "unknown_helper"),
            )

        install_activation.assert_called_once_with(bindings, ("activate_macos_bundle_id",))
        install_click.assert_called_once_with(bindings, ("dispatch_macos_click",))
        install_process.assert_called_once_with(bindings, ("quit_macos_bundle_id",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))



if __name__ == "__main__":
    unittest.main()
