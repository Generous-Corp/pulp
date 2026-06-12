#!/usr/bin/env python3
"""Tests for evidence command facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("evidence_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class EvidenceCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_evidence_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 0

        bindings = {"_evidence_cli": types.SimpleNamespace(cmd_evidence=runner)}
        for name in [
            "current_branch",
            "evidence_scope_header_line",
            "print_evidence_summary",
            "evidence_empty_line",
        ]:
            bindings[name] = object()

        args_obj = object()
        self.assertEqual(self.mod.cmd_evidence(bindings, args_obj), 0)
        self.assertEqual(captured["args"], (args_obj,))
        for name in [
            "current_branch",
            "evidence_scope_header_line",
            "print_evidence_summary",
            "evidence_empty_line",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])


if __name__ == "__main__":
    unittest.main()
