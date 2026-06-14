#!/usr/bin/env python3
"""Tests for cleanup plan command facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("cleanup_plan_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CleanupPlanCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_cleanup_plan_command_helpers(self):
        self.assertEqual(
            self.mod.CLEANUP_PLAN_COMMAND_EXPORTS,
            ("print_local_ci_cleanup_plan",),
        )

    def test_cleanup_plan_print_delegates_with_assembled_dependencies(self):
        captured = {}

        def plan_runner(*args, **kwargs):
            captured["plan_args"] = args
            captured["plan_kwargs"] = kwargs

        bindings = {
            "_cleanup_cli": types.SimpleNamespace(print_local_ci_cleanup_plan=plan_runner),
        }
        deps = {"cleanup_plan_lines_fn": object()}
        plan = {"remove": []}

        with mock.patch.object(self.mod, "cleanup_plan_command_dependencies", return_value=deps):
            self.mod.print_local_ci_cleanup_plan(bindings, plan, dry_run=True)

        self.assertEqual(captured["plan_args"], (plan,))
        self.assertTrue(captured["plan_kwargs"]["dry_run"])
        self.assertIs(captured["plan_kwargs"]["cleanup_plan_lines_fn"], deps["cleanup_plan_lines_fn"])

    def test_install_cleanup_plan_command_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_cleanup_plan_command_helpers(
                bindings,
                ("print_local_ci_cleanup_plan", "custom_cleanup_plan_command"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("print_local_ci_cleanup_plan",)),
                mock.call(bindings, self.mod.__dict__, ("custom_cleanup_plan_command",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
