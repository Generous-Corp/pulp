#!/usr/bin/env python3
"""Tests for Linux target command facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("linux_target_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxTargetCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.LINUX_TARGET_BUNDLE_EXPORTS,
            *self.mod.LINUX_TARGET_XVFB_COMMAND_EXPORTS,
            *self.mod.LINUX_TARGET_WINDOW_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.LINUX_TARGET_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_linux_target_command_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_linux_target_bundle_helpers") as install_bundle,
            mock.patch.object(self.mod, "install_linux_target_xvfb_command_helpers") as install_xvfb,
            mock.patch.object(self.mod, "install_linux_target_window_command_helpers") as install_window,
        ):
            self.mod.install_linux_target_command_helpers(
                bindings,
                (
                    "remote_linux_bundle_relpath",
                    "build_linux_xvfb_remote_command",
                    "build_linux_window_driver_remote_command",
                ),
            )

        install_bundle.assert_called_once_with(bindings, ("remote_linux_bundle_relpath",))
        install_xvfb.assert_called_once_with(bindings, ("build_linux_xvfb_remote_command",))
        install_window.assert_called_once_with(bindings, ("build_linux_window_driver_remote_command",))

    def test_install_linux_target_command_helpers_preserves_unknown_fallbacks(self) -> None:
        bindings = {"custom": object()}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_linux_target_command_helpers(bindings, ("custom",))

        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))



if __name__ == "__main__":
    unittest.main()
