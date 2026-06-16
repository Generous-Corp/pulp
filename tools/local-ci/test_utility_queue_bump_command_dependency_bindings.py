#!/usr/bin/env python3
"""Tests for queue bump utility command dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("utility_queue_bump_command_dependency_bindings.py")


class UtilityQueueBumpCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_utility_queue_bump_command_dependencies_bind_facade_dependencies(self) -> None:
        bindings = {
            "normalize_priority": object(),
            "bump_queue_command_job": object(),
            "bump_queue_command_result_line": object(),
        }

        deps = self.mod.utility_queue_bump_command_dependencies(bindings)

        self.assertIs(deps["normalize_priority_fn"], bindings["normalize_priority"])
        self.assertIs(deps["bump_queue_command_job_fn"], bindings["bump_queue_command_job"])
        self.assertIs(deps["bump_queue_command_result_line_fn"], bindings["bump_queue_command_result_line"])


if __name__ == "__main__":
    unittest.main()
