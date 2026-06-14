#!/usr/bin/env python3
"""Tests for desktop probe facade composition."""

from module_test_utils import load_module_from_path
import unittest
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
        calls = []

        def windows_install(bindings, names):
            calls.append(("windows", names))

        def doctor_install(bindings, names):
            calls.append(("doctor", names))

        def local_install(bindings, globals_obj, names):
            calls.append(("local", names))

        self.mod.install_desktop_windows_probe_helpers = windows_install
        self.mod.install_desktop_doctor_probe_helpers = doctor_install
        self.mod.install_local_helpers = local_install

        self.mod.install_desktop_probe_helpers(
            {},
            (
                "probe_windows_repo_checkout",
                "probe_webdriver_endpoint",
                "custom_probe_export",
            ),
        )

        self.assertEqual(
            calls,
            [
                ("windows", ("probe_windows_repo_checkout",)),
                ("doctor", ("probe_webdriver_endpoint",)),
                ("local", ("custom_probe_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
