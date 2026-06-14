#!/usr/bin/env python3
"""Tests for desktop probe facade composition."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopProbeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_desktop_probe_exports_compose_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_WINDOWS_PROBE_EXPORTS,
            *self.mod.DESKTOP_DOCTOR_PROBE_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_probe_helpers_routes_selected_groups(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_windows_probe_helpers") as windows,
            mock.patch.object(self.mod, "install_desktop_doctor_probe_helpers") as doctor,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_probe_helpers(
                bindings,
                (
                    "probe_windows_repo_checkout",
                    "probe_webdriver_endpoint",
                    "custom_probe_export",
                ),
            )

        windows.assert_called_once_with(bindings, ("probe_windows_repo_checkout",))
        doctor.assert_called_once_with(bindings, ("probe_webdriver_endpoint",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_probe_export",))


if __name__ == "__main__":
    unittest.main()
