#!/usr/bin/env python3
"""Tests for local-CI PR list command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("local_ci_pr_list_command_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiPrListCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_local_ci_pr_list_command_dependencies_preserve_facade_seams(self) -> None:
        bindings = {
            "gh_available": object(),
            "gh_pr_list_open": object(),
            "open_pr_list_lines": object(),
        }

        deps = self.mod.local_ci_pr_list_command_dependencies(bindings)

        self.assertIs(deps["gh_available_fn"], bindings["gh_available"])
        self.assertIs(deps["gh_pr_list_open_fn"], bindings["gh_pr_list_open"])
        self.assertIs(deps["open_pr_list_lines_fn"], bindings["open_pr_list_lines"])


if __name__ == "__main__":
    unittest.main()
