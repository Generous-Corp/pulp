#!/usr/bin/env python3
"""Tests for validation progress marker dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("execution_progress_marker_bindings.py")


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

if __name__ == "__main__":
    unittest.main()
