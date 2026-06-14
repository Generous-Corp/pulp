#!/usr/bin/env python3
"""Tests for cleanup command facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("cleanup_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CleanupCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_cleanup_command_helpers(self):
        self.assertEqual(
            self.mod.CLEANUP_COMMAND_EXPORTS,
            (
                "print_local_ci_state_footprint",
                "print_local_ci_cleanup_plan",
                "cmd_cleanup",
            ),
        )
        self.assertEqual(len(self.mod.CLEANUP_COMMAND_EXPORTS), len(set(self.mod.CLEANUP_COMMAND_EXPORTS)))

    def _bindings(self, runner_name: str, runner):
        bindings = {"_cleanup_cli": types.SimpleNamespace(**{runner_name: runner})}
        for name in [
            "local_ci_state_footprint",
            "state_footprint_lines",
            "cleanup_plan_lines",
            "load_queue",
            "collect_local_ci_cleanup_plan",
            "apply_local_ci_cleanup_plan",
            "print_local_ci_cleanup_plan",
            "print_local_ci_state_footprint",
            "format_size_bytes",
            "describe_path_for_cleanup",
        ]:
            bindings[name] = object()
        return bindings

    def test_cleanup_footprint_and_plan_bind_facade_dependencies(self):
        captured = {}

        def footprint_runner(**kwargs):
            captured["footprint"] = kwargs

        bindings = self._bindings("print_local_ci_state_footprint", footprint_runner)
        self.mod.print_local_ci_state_footprint(bindings, indent="  ")

        self.assertIs(captured["footprint"]["local_ci_state_footprint_fn"], bindings["local_ci_state_footprint"])
        self.assertIs(captured["footprint"]["state_footprint_lines_fn"], bindings["state_footprint_lines"])
        self.assertEqual(captured["footprint"]["indent"], "  ")

        def plan_runner(*args, **kwargs):
            captured["plan_args"] = args
            captured["plan_kwargs"] = kwargs

        bindings = self._bindings("print_local_ci_cleanup_plan", plan_runner)
        plan = {"remove": []}
        self.mod.print_local_ci_cleanup_plan(bindings, plan, dry_run=True)

        self.assertEqual(captured["plan_args"], (plan,))
        self.assertTrue(captured["plan_kwargs"]["dry_run"])
        self.assertIs(captured["plan_kwargs"]["cleanup_plan_lines_fn"], bindings["cleanup_plan_lines"])

    def test_cmd_cleanup_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 9

        bindings = self._bindings("cmd_cleanup", runner)
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


if __name__ == "__main__":
    unittest.main()
