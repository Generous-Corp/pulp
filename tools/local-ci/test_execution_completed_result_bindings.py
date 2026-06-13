#!/usr/bin/env python3
"""Tests for completed validation result dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("execution_completed_result_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionCompletedResultBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_completed_job_result_binds_time_and_provenance(self):
        captured = {}

        def completed_job_result(job, results, *, completed_at, provenance):
            captured["completed"] = (job, results, completed_at, provenance)
            return {"overall": "pass"}

        execution = types.SimpleNamespace(
            completed_job_result=completed_job_result,
            sorted_target_results=lambda results: list(reversed(results)),
        )
        bindings = {
            "_execution": execution,
            "now_iso": lambda: "now",
            "normalize_provenance": lambda provenance: {"normalized": provenance},
        }

        self.assertEqual(self.mod.completed_job_result(bindings, {"id": "job", "provenance": "p"}, [{"target": "mac"}]), {"overall": "pass"})
        self.assertEqual(captured["completed"][2], "now")
        self.assertEqual(captured["completed"][3], {"normalized": "p"})
        self.assertEqual(self.mod.sorted_target_results(bindings, [1, 2]), [2, 1])

    def test_completed_result_exports_match_helpers(self):
        expected = ("completed_job_result", "sorted_target_results")

        self.assertEqual(self.mod.EXECUTION_COMPLETED_RESULT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_completed_result_installer_wires_selected_exports(self):
        execution = types.SimpleNamespace(sorted_target_results=lambda results: list(reversed(results)))
        bindings = {"_execution": execution}

        self.mod.install_execution_completed_result_helpers(bindings, ("sorted_target_results",))

        self.assertEqual(bindings["sorted_target_results"]([1, 2]), [2, 1])
        self.assertNotIn("completed_job_result", bindings)

    def test_install_execution_completed_result_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_execution_completed_result_helper = lambda _bindings: "future"

        self.mod.install_execution_completed_result_helpers(bindings, ("future_execution_completed_result_helper",))

        self.assertEqual(bindings["future_execution_completed_result_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
