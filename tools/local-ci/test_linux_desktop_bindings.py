#!/usr/bin/env python3
"""Tests for Linux desktop facade composition."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
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
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_linux_desktop_action_helpers") as action,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_linux_desktop_helpers(
                bindings,
                ("fetch_ssh_artifact", "run_linux_xvfb_remote_action", "custom_linux_desktop_export"),
            )

        action.assert_called_once_with(bindings, ("run_linux_xvfb_remote_action",))
        install_local.assert_has_calls(
            [
                mock.call(bindings, self.mod.__dict__, ("fetch_ssh_artifact",)),
                mock.call(bindings, self.mod.__dict__, ("custom_linux_desktop_export",)),
            ]
        )


if __name__ == "__main__":
    unittest.main()
