#!/usr/bin/env python3
"""Tests for cleanup command execution facade bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("cleanup_run_command_bindings.py")


class CleanupRunCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_cleanup_run_command_helpers(self):
        self.assertEqual(self.mod.CLEANUP_RUN_COMMAND_EXPORTS, ("cmd_cleanup",))

    def test_cmd_cleanup_delegates_with_assembled_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 9

        bindings = {"_cleanup_cli": types.SimpleNamespace(cmd_cleanup=runner)}
        deps = {
            "load_queue_fn": object(),
            "collect_cleanup_plan_fn": object(),
            "apply_cleanup_plan_fn": object(),
            "print_cleanup_plan_fn": object(),
            "print_state_footprint_fn": object(),
            "format_size_fn": object(),
            "describe_path_fn": object(),
        }

        args_obj = object()
        with mock.patch.object(self.mod, "cleanup_run_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_cleanup(bindings, args_obj), 9)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], deps)

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
