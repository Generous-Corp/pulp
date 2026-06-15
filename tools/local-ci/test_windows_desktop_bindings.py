#!/usr/bin/env python3
"""Tests for Windows desktop facade composition."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("windows_desktop_bindings.py")


class WindowsDesktopBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_windows_desktop_exports_are_composed_from_action_exports(self):
        self.assertEqual(self.mod.WINDOWS_DESKTOP_EXPORTS, self.mod.WINDOWS_DESKTOP_ACTION_EXPORTS)
        self.assertEqual(len(self.mod.WINDOWS_DESKTOP_EXPORTS), len(set(self.mod.WINDOWS_DESKTOP_EXPORTS)))

    def test_install_windows_desktop_helpers_routes_action_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_windows_desktop_action_helpers") as action,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_windows_desktop_helpers(
                bindings,
                ("run_windows_session_agent_action", "custom_windows_desktop_export"),
            )

        action.assert_called_once_with(bindings, ("run_windows_session_agent_action",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_windows_desktop_export",))


if __name__ == "__main__":
    unittest.main()
