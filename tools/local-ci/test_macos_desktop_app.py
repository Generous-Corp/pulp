#!/usr/bin/env python3
"""No-network tests for macOS app helper functions."""

from __future__ import annotations

from pathlib import Path
import plistlib
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("macos_desktop_app.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacOSDesktopAppTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_bundle_detection_and_plist_bundle_id(self) -> None:
        app_binary = self.root / "Demo.app" / "Contents" / "MacOS" / "Demo"
        app_binary.parent.mkdir(parents=True)
        app_binary.write_text("#!/bin/sh\n")

        self.assertIsNone(self.mod.detect_macos_app_bundle(None))
        self.assertIsNone(self.mod.detect_macos_app_bundle(""))
        self.assertEqual(self.mod.detect_macos_app_bundle(str(app_binary)), self.root / "Demo.app")
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Missing.app"))

        info_plist = self.root / "Demo.app" / "Contents" / "Info.plist"
        info_plist.write_bytes(plistlib.dumps({"CFBundleIdentifier": "com.example.demo"}))
        self.assertEqual(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"), "com.example.demo")

        info_plist.write_text("not a plist")
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"))

    def test_probe_path_uses_script_dir(self) -> None:
        self.assertEqual(self.mod.macos_window_probe_path(self.root), self.root / "macos_window_probe.swift")


if __name__ == "__main__":
    unittest.main()
