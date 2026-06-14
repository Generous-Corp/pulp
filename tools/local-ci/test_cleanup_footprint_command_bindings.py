#!/usr/bin/env python3
"""Tests for cleanup footprint command facade bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("cleanup_footprint_command_bindings.py")


class CleanupFootprintCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_cleanup_footprint_command_helpers(self):
        self.assertEqual(
            self.mod.CLEANUP_FOOTPRINT_COMMAND_EXPORTS,
            ("print_local_ci_state_footprint",),
        )

    def test_cleanup_footprint_delegates_with_assembled_dependencies(self):
        captured = {}

        def footprint_runner(**kwargs):
            captured["footprint"] = kwargs

        bindings = {
            "_cleanup_cli": types.SimpleNamespace(print_local_ci_state_footprint=footprint_runner),
        }
        deps = {
            "local_ci_state_footprint_fn": object(),
            "state_footprint_lines_fn": object(),
        }

        with mock.patch.object(self.mod, "cleanup_footprint_command_dependencies", return_value=deps):
            self.mod.print_local_ci_state_footprint(bindings, indent="  ")

        self.assertIs(captured["footprint"]["local_ci_state_footprint_fn"], deps["local_ci_state_footprint_fn"])
        self.assertIs(captured["footprint"]["state_footprint_lines_fn"], deps["state_footprint_lines_fn"])
        self.assertEqual(captured["footprint"]["indent"], "  ")

    def test_install_cleanup_footprint_command_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_cleanup_footprint_command_helpers(
                bindings,
                ("print_local_ci_state_footprint", "custom_footprint"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("print_local_ci_state_footprint",)),
                mock.call(bindings, self.mod.__dict__, ("custom_footprint",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
