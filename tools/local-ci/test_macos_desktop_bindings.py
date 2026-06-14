#!/usr/bin/env python3
"""Tests for macOS desktop facade composition."""

from module_test_utils import load_module_from_path
import unittest
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
        calls = []

        def smoke_install(bindings, names):
            calls.append(("smoke", names))

        def local_install(bindings, globals_obj, names):
            calls.append(("local", names))

        self.mod.install_macos_desktop_smoke_helpers = smoke_install
        self.mod.install_local_helpers = local_install

        self.mod.install_macos_desktop_helpers(
            {},
            ("run_macos_local_smoke", "custom_macos_desktop_export"),
        )

        self.assertEqual(
            calls,
            [
                ("smoke", ("run_macos_local_smoke",)),
                ("local", ("custom_macos_desktop_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
