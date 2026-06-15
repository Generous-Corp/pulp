#!/usr/bin/env python3
"""No-network tests for desktop action label helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_action_label.py")


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
