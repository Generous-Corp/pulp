#!/usr/bin/env python3
"""Tests for utility command facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("utility_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class UtilityCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_compose_focused_command_groups(self):
        self.assertEqual(
            self.mod.UTILITY_COMMAND_EXPORTS,
            (
                *self.mod.CLEANUP_COMMAND_EXPORTS,
                *self.mod.UTILITY_QUEUE_COMMAND_EXPORTS,
                *self.mod.LOGS_COMMAND_EXPORTS,
                *self.mod.EVIDENCE_COMMAND_EXPORTS,
            ),
        )
        self.assertEqual(len(self.mod.UTILITY_COMMAND_EXPORTS), len(set(self.mod.UTILITY_COMMAND_EXPORTS)))

    def _bindings(self, runner):
        bindings = {
            "_queue_commands_cli": types.SimpleNamespace(cmd_bump=runner),
            "normalize_priority": object(),
            "bump_queue_command_job": object(),
            "bump_queue_command_result_line": object(),
        }
        return bindings

    def test_install_utility_command_helpers_wires_named_exports(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 8

        bindings = self._bindings(runner)

        self.mod.install_utility_command_helpers(bindings, names=("cmd_bump",))

        args_obj = object()
        self.assertEqual(bindings["cmd_bump"](args_obj), 8)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["normalize_priority_fn"], bindings["normalize_priority"])
        self.assertEqual(bindings["cmd_bump"].__name__, "cmd_bump")


if __name__ == "__main__":
    unittest.main()
