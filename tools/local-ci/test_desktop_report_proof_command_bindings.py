#!/usr/bin/env python3
"""Tests for desktop proof report command bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_report_proof_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopReportProofCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_proof_command_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.DESKTOP_REPORT_PROOF_COMMAND_EXPORTS, ("cmd_desktop_proof",))

    def test_proof_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 6

        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(cmd_desktop_proof=runner),
        }
        deps = {
            "load_config_fn": object(),
            "desktop_proof_summaries_fn": object(),
            "desktop_proof_empty_line_fn": object(),
            "desktop_proof_lines_fn": object(),
            "short_sha_fn": object(),
        }

        args_obj = object()
        with mock.patch.object(self.mod, "desktop_report_proof_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_desktop_proof(bindings, args_obj), 6)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], deps)


if __name__ == "__main__":
    unittest.main()
