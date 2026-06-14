#!/usr/bin/env python3
"""Tests for cleanup plan command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("cleanup_plan_command_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CleanupPlanCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cleanup_plan_command_dependencies_preserve_plan_line_seam(self) -> None:
        bindings = {"cleanup_plan_lines": object()}

        deps = self.mod.cleanup_plan_command_dependencies(bindings)

        self.assertIs(deps["cleanup_plan_lines_fn"], bindings["cleanup_plan_lines"])


if __name__ == "__main__":
    unittest.main()
