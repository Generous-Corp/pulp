#!/usr/bin/env python3
"""Tests for queue cancel utility command dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("utility_queue_cancel_command_dependency_bindings.py")


class UtilityQueueCancelCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_utility_queue_cancel_command_dependencies_bind_facade_dependencies(self) -> None:
        bindings = {
            "cancel_queue_command_job": object(),
            "cancel_queue_command_result_line": object(),
        }

        deps = self.mod.utility_queue_cancel_command_dependencies(bindings)

        self.assertIs(deps["cancel_queue_command_job_fn"], bindings["cancel_queue_command_job"])
        self.assertIs(deps["cancel_queue_command_result_line_fn"], bindings["cancel_queue_command_result_line"])


if __name__ == "__main__":
    unittest.main()
