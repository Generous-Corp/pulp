#!/usr/bin/env python3
"""Tests for remote exact-source preparation compatibility bindings."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_exact_source_remote_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopExactSourceRemoteBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_remote_facade_reexports_focused_helpers(self):
        self.assertEqual(self.mod.prepare_linux_exact_sha_source.__module__, "desktop_exact_source_linux_bindings")
        self.assertEqual(self.mod.prepare_windows_exact_sha_source.__module__, "desktop_exact_source_windows_bindings")


if __name__ == "__main__":
    unittest.main()
