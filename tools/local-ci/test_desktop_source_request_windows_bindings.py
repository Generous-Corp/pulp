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

    def test_windows_installer_wires_selected_helper(self):
        bindings = {
            "_source_prep": types.SimpleNamespace(split_windows_prepare_commands=lambda command: ["one", "two"]),
        }

        self.mod.install_desktop_source_request_windows_helpers(bindings, ("split_windows_prepare_commands",))

        self.assertEqual(bindings["split_windows_prepare_commands"]("one;two"), ["one", "two"])
        self.assertNotIn("validate_windows_prepare_commands", bindings)

    def test_windows_installer_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_desktop_source_request_windows_helper = lambda _bindings: "future"

        self.mod.install_desktop_source_request_windows_helpers(
            bindings,
            ("future_desktop_source_request_windows_helper",),
        )

        self.assertEqual(bindings["future_desktop_source_request_windows_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
