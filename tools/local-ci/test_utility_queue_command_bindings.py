#!/usr/bin/env python3
"""Tests for queue utility command facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("utility_queue_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class UtilityQueueCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_queue_command_helpers(self):
        self.assertEqual(
            self.mod.UTILITY_QUEUE_COMMAND_EXPORTS,
            (
                "cmd_bump",
                "cmd_cancel",
            ),
        )
        self.assertEqual(len(self.mod.UTILITY_QUEUE_COMMAND_EXPORTS), len(set(self.mod.UTILITY_QUEUE_COMMAND_EXPORTS)))

    def _bindings(self, runner_name: str, runner):
        bindings = {"_queue_commands_cli": types.SimpleNamespace(**{runner_name: runner})}
        for name in [
            "normalize_priority",
            "bump_queue_command_job",
            "bump_queue_command_result_line",
            "cancel_queue_command_job",
            "cancel_queue_command_result_line",
        ]:
            bindings[name] = object()
        return bindings

    def test_queue_commands_bind_facade_dependencies(self):
        cases = [
            (
                "cmd_bump",
                self.mod.cmd_bump,
                ["normalize_priority", "bump_queue_command_job", "bump_queue_command_result_line"],
            ),
            (
                "cmd_cancel",
                self.mod.cmd_cancel,
                ["cancel_queue_command_job", "cancel_queue_command_result_line"],
            ),
        ]
        for runner_name, wrapper, dependency_names in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*args, **kwargs):
                    captured["args"] = args
                    captured["kwargs"] = kwargs
                    return 4

                bindings = self._bindings(runner_name, runner)
                args_obj = object()
                self.assertEqual(wrapper(bindings, args_obj), 4)
                self.assertEqual(captured["args"], (args_obj,))
                for name in dependency_names:
                    self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])


if __name__ == "__main__":
    unittest.main()
