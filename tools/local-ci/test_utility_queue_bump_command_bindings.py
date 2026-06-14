#!/usr/bin/env python3
"""Tests for queue bump utility command facade bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("utility_queue_bump_command_bindings.py")


class UtilityQueueBumpCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_queue_bump_command_helpers(self):
        self.assertEqual(self.mod.UTILITY_QUEUE_BUMP_COMMAND_EXPORTS, ("cmd_bump",))

    def test_cmd_bump_delegates_with_assembled_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 4

        bindings = {
            "_queue_commands_cli": types.SimpleNamespace(cmd_bump=runner),
            "normalize_priority": object(),
            "bump_queue_command_job": object(),
            "bump_queue_command_result_line": object(),
        }
        args_obj = object()
        deps = {"normalize_priority_fn": object(), "bump_queue_command_job_fn": object()}

        with mock.patch.object(self.mod, "utility_queue_bump_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_bump(bindings, args_obj), 4)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["normalize_priority_fn"], deps["normalize_priority_fn"])
        self.assertIs(captured["kwargs"]["bump_queue_command_job_fn"], deps["bump_queue_command_job_fn"])

    def test_install_utility_queue_bump_command_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_utility_queue_bump_command_helpers(
                bindings,
                ("cmd_bump", "custom_bump_command"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("cmd_bump",)),
                mock.call(bindings, self.mod.__dict__, ("custom_bump_command",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
