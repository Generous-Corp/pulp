#!/usr/bin/env python3
"""Tests for desktop proof report command bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_report_proof_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopReportProofCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_proof_command_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.DESKTOP_REPORT_PROOF_COMMAND_EXPORTS, ("cmd_desktop_proof",))

    def test_proof_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 6

        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(cmd_desktop_proof=runner),
            "_desktop_cli": types.SimpleNamespace(
                desktop_proof_empty_line=object(),
                desktop_proof_lines=object(),
            ),
            "load_config": object(),
            "desktop_proof_summaries": object(),
            "short_sha": object(),
        }

        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_proof(bindings, args_obj), 6)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["desktop_proof_summaries_fn"], bindings["desktop_proof_summaries"])
        self.assertIs(captured["kwargs"]["desktop_proof_empty_line_fn"], bindings["_desktop_cli"].desktop_proof_empty_line)
        self.assertIs(captured["kwargs"]["desktop_proof_lines_fn"], bindings["_desktop_cli"].desktop_proof_lines)
        self.assertIs(captured["kwargs"]["short_sha_fn"], bindings["short_sha"])


if __name__ == "__main__":
    unittest.main()
