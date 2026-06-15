#!/usr/bin/env python3
"""Tests for macOS window facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("macos_window_bindings.py")


class MacosWindowBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_window_exports_compose_focused_groups(self) -> None:
        expected = (
            *self.mod.MACOS_WINDOW_APP_EXPORTS,
            *self.mod.MACOS_WINDOW_PROBE_EXPORTS,
            *self.mod.MACOS_WINDOW_ACTION_EXPORTS,
        )

        self.assertEqual(self.mod.MACOS_WINDOW_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_macos_window_helpers_routes_selected_groups_and_unknown_exports(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_macos_window_app_helpers") as install_app,
            mock.patch.object(self.mod, "install_macos_window_probe_helpers") as install_probe,
            mock.patch.object(self.mod, "install_macos_window_action_helpers") as install_action,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_macos_window_helpers(
                bindings,
                (
                    "detect_macos_app_bundle",
                    "macos_accessibility_trusted",
                    "activate_macos_bundle_id",
                    "custom",
                ),
            )

        install_app.assert_called_once_with(bindings, ("detect_macos_app_bundle",))
        install_probe.assert_called_once_with(bindings, ("macos_accessibility_trusted",))
        install_action.assert_called_once_with(bindings, ("activate_macos_bundle_id",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))



if __name__ == "__main__":
    unittest.main()
