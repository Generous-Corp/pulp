#!/usr/bin/env python3
"""Tests for Linux desktop facade composition."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("linux_desktop_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxDesktopBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_linux_desktop_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.LINUX_DESKTOP_ARTIFACT_EXPORTS,
            *self.mod.LINUX_DESKTOP_ACTION_EXPORTS,
        )

        self.assertEqual(self.mod.LINUX_DESKTOP_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_linux_desktop_helpers_routes_artifact_action_and_unknown_exports(self):
        calls = []

        def action_install(bindings, names):
            calls.append(("action", names))

        def local_install(bindings, globals_obj, names):
            calls.append(("local", names))

        self.mod.install_linux_desktop_action_helpers = action_install
        self.mod.install_local_helpers = local_install

        self.mod.install_linux_desktop_helpers(
            {},
            ("fetch_ssh_artifact", "run_linux_xvfb_remote_action", "custom_linux_desktop_export"),
        )

        self.assertEqual(
            calls,
            [
                ("local", ("fetch_ssh_artifact",)),
                ("action", ("run_linux_xvfb_remote_action",)),
                ("local", ("custom_linux_desktop_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
