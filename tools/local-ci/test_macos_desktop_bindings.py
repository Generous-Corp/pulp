#!/usr/bin/env python3
"""Tests for macOS desktop facade composition."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("macos_desktop_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosDesktopBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_macos_desktop_exports_are_composed_from_smoke_exports(self):
        self.assertEqual(self.mod.MACOS_DESKTOP_EXPORTS, self.mod.MACOS_DESKTOP_SMOKE_EXPORTS)
        self.assertEqual(len(self.mod.MACOS_DESKTOP_EXPORTS), len(set(self.mod.MACOS_DESKTOP_EXPORTS)))

    def test_install_macos_desktop_helpers_routes_smoke_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_macos_desktop_smoke_helpers") as smoke,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_macos_desktop_helpers(
                bindings,
                ("run_macos_local_smoke", "custom_macos_desktop_export"),
            )

        smoke.assert_called_once_with(bindings, ("run_macos_local_smoke",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_macos_desktop_export",))


if __name__ == "__main__":
    unittest.main()
