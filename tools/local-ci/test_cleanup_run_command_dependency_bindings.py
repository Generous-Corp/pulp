#!/usr/bin/env python3
"""Tests for cleanup command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("cleanup_run_command_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CleanupRunCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cleanup_run_command_dependencies_preserve_cleanup_seams(self) -> None:
        bindings = {}
        expected = {
            "load_queue": "load_queue_fn",
            "collect_local_ci_cleanup_plan": "collect_cleanup_plan_fn",
            "apply_local_ci_cleanup_plan": "apply_cleanup_plan_fn",
            "print_local_ci_cleanup_plan": "print_cleanup_plan_fn",
            "print_local_ci_state_footprint": "print_state_footprint_fn",
            "format_size_bytes": "format_size_fn",
            "describe_path_for_cleanup": "describe_path_fn",
        }
        for name in expected:
            bindings[name] = object()

        deps = self.mod.cleanup_run_command_dependencies(bindings)

        self.assertEqual(set(deps), set(expected.values()))
        for binding_name, dependency_name in expected.items():
            self.assertIs(deps[dependency_name], bindings[binding_name])


if __name__ == "__main__":
    unittest.main()
