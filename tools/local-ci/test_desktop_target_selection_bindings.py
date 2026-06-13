#!/usr/bin/env python3
"""Tests for desktop target selection facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_target_selection_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopTargetSelectionBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_target_selection_exports_match_wrappers(self) -> None:
        expected = ("resolve_desktop_target",)

        self.assertEqual(self.mod.DESKTOP_TARGET_SELECTION_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_resolve_desktop_target_preserves_selection_errors(self) -> None:
        config = {
            "desktop_automation": {
                "targets": {
                    "mac": {"adapter": "macos-local"},
                    "linux": {"adapter": "linux-xvfb", "enabled": False},
                }
            }
        }

        self.assertEqual(
            self.mod.resolve_desktop_target({}, config, "mac"),
            {"adapter": "macos-local"},
        )
        with self.assertRaisesRegex(ValueError, "Unknown desktop target 'windows'"):
            self.mod.resolve_desktop_target({}, config, "windows")
        with self.assertRaisesRegex(ValueError, "Desktop target 'linux' is disabled"):
            self.mod.resolve_desktop_target({}, config, "linux")

    def test_install_target_selection_helpers_wires_named_exports(self) -> None:
        config = {"desktop_automation": {"targets": {"mac": {"adapter": "macos-local"}}}}
        bindings = {}

        self.mod.install_desktop_target_selection_helpers(bindings)

        self.assertEqual(bindings["resolve_desktop_target"](config, "mac"), {"adapter": "macos-local"})

    def test_install_desktop_target_selection_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_desktop_target_selection_helper = lambda _bindings: "future"

        self.mod.install_desktop_target_selection_helpers(bindings, ("future_desktop_target_selection_helper",))

        self.assertEqual(bindings["future_desktop_target_selection_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
