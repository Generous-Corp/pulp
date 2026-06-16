#!/usr/bin/env python3
"""Tests for local validation command dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("execution_local_command_bindings.py")


class ExecutionLocalCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_local_command_exports_match_facade_helpers(self):
        expected = ("local_validation_command",)

        self.assertEqual(self.mod.EXECUTION_LOCAL_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_local_validation_command_delegates_to_execution_module(self):
        execution = types.SimpleNamespace(
            local_validation_command=lambda job, exclude_tests="": ([job["id"], exclude_tests], job.get("validation", "full")),
        )
        bindings = {"_execution": execution}

        self.assertEqual(
            self.mod.local_validation_command(bindings, {"id": "job", "validation": "smoke"}, "slow"),
            (["job", "slow"], "smoke"),
        )

if __name__ == "__main__":
    unittest.main()
