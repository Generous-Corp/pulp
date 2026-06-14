#!/usr/bin/env python3
"""Tests for Windows desktop facade composition."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("windows_desktop_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsDesktopBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_windows_desktop_exports_are_composed_from_action_exports(self):
        self.assertEqual(self.mod.WINDOWS_DESKTOP_EXPORTS, self.mod.WINDOWS_DESKTOP_ACTION_EXPORTS)
        self.assertEqual(len(self.mod.WINDOWS_DESKTOP_EXPORTS), len(set(self.mod.WINDOWS_DESKTOP_EXPORTS)))

    def test_install_windows_desktop_helpers_routes_action_and_unknown_exports(self):
        calls = []

        def action_install(bindings, names):
            calls.append(("action", names))

        def local_install(bindings, globals_obj, names):
            calls.append(("local", names))

        self.mod.install_windows_desktop_action_helpers = action_install
        self.mod.install_local_helpers = local_install

        self.mod.install_windows_desktop_helpers(
            {},
            ("run_windows_session_agent_action", "custom_windows_desktop_export"),
        )

        self.assertEqual(
            calls,
            [
                ("action", ("run_windows_session_agent_action",)),
                ("local", ("custom_windows_desktop_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
