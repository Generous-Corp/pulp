#!/usr/bin/env python3
"""Tests for cleanup command execution facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("cleanup_run_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CleanupRunCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_cleanup_run_command_helpers(self):
        self.assertEqual(self.mod.CLEANUP_RUN_COMMAND_EXPORTS, ("cmd_cleanup",))

    def test_cmd_cleanup_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 9

        bindings = {"_cleanup_cli": types.SimpleNamespace(cmd_cleanup=runner)}
        for name in [
            "load_queue",
            "collect_local_ci_cleanup_plan",
            "apply_local_ci_cleanup_plan",
            "print_local_ci_cleanup_plan",
            "print_local_ci_state_footprint",
            "format_size_bytes",
            "describe_path_for_cleanup",
        ]:
            bindings[name] = object()

        args_obj = object()
        self.assertEqual(self.mod.cmd_cleanup(bindings, args_obj), 9)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_queue_fn"], bindings["load_queue"])
        self.assertIs(captured["kwargs"]["collect_cleanup_plan_fn"], bindings["collect_local_ci_cleanup_plan"])
        self.assertIs(captured["kwargs"]["apply_cleanup_plan_fn"], bindings["apply_local_ci_cleanup_plan"])
        self.assertIs(captured["kwargs"]["print_cleanup_plan_fn"], bindings["print_local_ci_cleanup_plan"])
        self.assertIs(captured["kwargs"]["print_state_footprint_fn"], bindings["print_local_ci_state_footprint"])
        self.assertIs(captured["kwargs"]["format_size_fn"], bindings["format_size_bytes"])
        self.assertIs(captured["kwargs"]["describe_path_fn"], bindings["describe_path_for_cleanup"])

    def test_install_cleanup_run_command_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_cleanup_run_command_helpers(bindings, ("cmd_cleanup", "custom_cleanup_run"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("cmd_cleanup",)),
                mock.call(bindings, self.mod.__dict__, ("custom_cleanup_run",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
