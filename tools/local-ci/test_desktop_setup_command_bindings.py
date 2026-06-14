#!/usr/bin/env python3
"""Tests for desktop setup command facade bindings."""

from __future__ import annotations

from pathlib import Path
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_setup_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSetupCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_INSTALL_COMMAND_EXPORTS,
            *self.mod.DESKTOP_DOCTOR_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_SETUP_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_setup_command_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_install_command_helpers") as install_install,
            mock.patch.object(self.mod, "install_desktop_doctor_command_helpers") as install_doctor,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_setup_command_helpers(
                bindings,
                ("cmd_desktop_install", "cmd_desktop_doctor", "custom_setup_command"),
            )

        install_install.assert_called_once_with(bindings, ("cmd_desktop_install",))
        install_doctor.assert_called_once_with(bindings, ("cmd_desktop_doctor",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_setup_command",))


if __name__ == "__main__":
    unittest.main()
