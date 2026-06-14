#!/usr/bin/env python3
"""Tests for validation result dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("execution_result_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionResultBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_result_installer_wires_selected_exports(self):
        execution = types.SimpleNamespace(
            unreachable_target_result=lambda target, detail="Host unreachable": {"target": target, "detail": detail},
            sorted_target_results=lambda results: list(reversed(results)),
        )
        bindings = {"_execution": execution}

        self.mod.install_execution_result_helpers(bindings, ("unreachable_target_result", "sorted_target_results"))

        self.assertEqual(bindings["unreachable_target_result"]("linux"), {"target": "linux", "detail": "Host unreachable"})
        self.assertEqual(bindings["sorted_target_results"]([1, 2]), [2, 1])
        self.assertNotIn("validation_result_from_run", bindings)

    def test_result_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.EXECUTION_TARGET_RESULT_EXPORTS,
            *self.mod.EXECUTION_COMPLETED_RESULT_EXPORTS,
            *self.mod.EXECUTION_TASK_RESULT_EXPORTS,
        )

        self.assertEqual(self.mod.EXECUTION_RESULT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_result_installer_routes_focused_groups_and_fallback(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_execution_target_result_helpers") as install_target,
            mock.patch.object(self.mod, "install_execution_completed_result_helpers") as install_completed,
            mock.patch.object(self.mod, "install_execution_task_result_helpers") as install_task,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_execution_result_helpers(
                bindings,
                ("validation_error_result", "completed_job_result", "run_target_tasks", "unknown_helper"),
            )

        install_target.assert_called_once_with(bindings, ("validation_error_result",))
        install_completed.assert_called_once_with(bindings, ("completed_job_result",))
        install_task.assert_called_once_with(bindings, ("run_target_tasks",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
