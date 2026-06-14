#!/usr/bin/env python3
"""Tests for validation target-task result dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("execution_task_result_bindings.py")


class ExecutionTaskResultBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_run_target_tasks_binds_exception_result(self):
        captured = {}

        def run_target_tasks(tasks, *, exception_result_fn, on_target_complete):
            captured["tasks"] = (tasks, exception_result_fn, on_target_complete)
            return [{"target": "mac"}]

        bindings = {
            "_execution": types.SimpleNamespace(run_target_tasks=run_target_tasks),
            "target_exception_result": object(),
        }

        complete = object()
        self.assertEqual(self.mod.run_target_tasks(bindings, [("mac", lambda: {})], on_target_complete=complete), [{"target": "mac"}])
        self.assertIs(captured["tasks"][1], bindings["target_exception_result"])
        self.assertIs(captured["tasks"][2], complete)

    def test_task_result_exports_match_helpers(self):
        expected = ("run_target_tasks",)

        self.assertEqual(self.mod.EXECUTION_TASK_RESULT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

if __name__ == "__main__":
    unittest.main()
