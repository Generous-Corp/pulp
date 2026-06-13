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


if __name__ == "__main__":
    unittest.main()
