#!/usr/bin/env python3
"""No-network tests for desktop action label helpers."""

from __future__ import annotations

import pathlib
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("desktop_action_label.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopActionLabelTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_default_desktop_label_matches_bundle_or_command(self) -> None:
        self.assertEqual(self.mod.default_desktop_label(None), "desktop-run")
        self.assertEqual(self.mod.default_desktop_label(""), "desktop-run")
        self.assertEqual(self.mod.default_desktop_label("/Applications/TextEdit.app/Contents/MacOS/TextEdit"), "TextEdit")
        self.assertEqual(self.mod.default_desktop_label(None, bundle_id="com.apple.TextEdit"), "TextEdit")
        self.assertEqual(self.mod.default_desktop_label(None, bundle_id="com.example."), "com.example.")


if __name__ == "__main__":
    unittest.main()
