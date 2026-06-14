#!/usr/bin/env python3
"""Tests for Windows source prepare-command dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_source_request_windows_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSourceRequestWindowsBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_windows_exports_match_wrappers(self):
        expected = (
            "split_windows_prepare_commands",
            "validate_windows_prepare_commands",
        )

        self.assertEqual(self.mod.DESKTOP_SOURCE_REQUEST_WINDOWS_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_windows_helpers_delegate(self):
        captured = {}

        def split_windows(command):
            captured["split"] = command
            return ["one", "two"]

        def validate_windows(commands):
            captured["validate"] = commands

        bindings = {
            "_source_prep": types.SimpleNamespace(
                split_windows_prepare_commands=split_windows,
                validate_windows_prepare_commands=validate_windows,
            )
        }

        self.assertEqual(self.mod.split_windows_prepare_commands(bindings, "one;two"), ["one", "two"])
        self.assertEqual(captured["split"], "one;two")
        self.mod.validate_windows_prepare_commands(bindings, ["one"])
        self.assertEqual(captured["validate"], ["one"])

if __name__ == "__main__":
    unittest.main()
