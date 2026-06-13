#!/usr/bin/env python3
"""Tests for validation progress marker dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("execution_progress_marker_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionProgressMarkerBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_progress_marker_exports_match_helpers(self):
        expected = ("parse_progress_marker",)

        self.assertEqual(self.mod.EXECUTION_PROGRESS_MARKER_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_parse_progress_marker_delegates_to_execution_module(self):
        bindings = {
            "_execution": types.SimpleNamespace(parse_progress_marker=lambda line: {"line": line}),
        }

        self.assertEqual(self.mod.parse_progress_marker(bindings, "line"), {"line": "line"})

    def test_progress_marker_installer_wires_selected_exports(self):
        bindings = {
            "_execution": types.SimpleNamespace(parse_progress_marker=lambda line: {"line": line}),
        }

        self.mod.install_execution_progress_marker_helpers(bindings, ("parse_progress_marker",))

        self.assertEqual(bindings["parse_progress_marker"]("line"), {"line": "line"})


if __name__ == "__main__":
    unittest.main()
