#!/usr/bin/env python3
"""Tests for cleanup plan apply/display dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("cleanup_plan_apply_dependency_bindings.py")


class CleanupPlanApplyDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cleanup_plan_lines_dependencies_preserve_formatting_seams(self) -> None:
        bindings = {
            "format_size_bytes": object(),
            "describe_path_for_cleanup": object(),
        }

        deps = self.mod.cleanup_plan_lines_dependencies(bindings)

        self.assertIs(deps["format_size_fn"], bindings["format_size_bytes"])
        self.assertIs(deps["describe_path_fn"], bindings["describe_path_for_cleanup"])


if __name__ == "__main__":
    unittest.main()
