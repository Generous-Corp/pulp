#!/usr/bin/env python3
"""Tests for cleanup footprint command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("cleanup_footprint_command_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CleanupFootprintCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cleanup_footprint_command_dependencies_preserve_facade_seams(self) -> None:
        bindings = {
            "local_ci_state_footprint": object(),
            "state_footprint_lines": object(),
        }

        deps = self.mod.cleanup_footprint_command_dependencies(bindings)

        self.assertIs(deps["local_ci_state_footprint_fn"], bindings["local_ci_state_footprint"])
        self.assertIs(deps["state_footprint_lines_fn"], bindings["state_footprint_lines"])


if __name__ == "__main__":
    unittest.main()
