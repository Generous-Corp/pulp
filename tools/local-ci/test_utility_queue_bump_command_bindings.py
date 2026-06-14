#!/usr/bin/env python3
"""Tests for queue bump utility command facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("utility_queue_bump_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class UtilityQueueBumpCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_queue_bump_command_helpers(self):
        self.assertEqual(self.mod.UTILITY_QUEUE_BUMP_COMMAND_EXPORTS, ("cmd_bump",))

    def test_cmd_bump_binds_facade_dependencies(self):
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

        self.assertEqual(self.mod.cmd_bump(bindings, args_obj), 4)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["normalize_priority_fn"], bindings["normalize_priority"])
        self.assertIs(captured["kwargs"]["bump_queue_command_job_fn"], bindings["bump_queue_command_job"])
        self.assertIs(
            captured["kwargs"]["bump_queue_command_result_line_fn"],
            bindings["bump_queue_command_result_line"],
        )

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
