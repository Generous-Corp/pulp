#!/usr/bin/env python3
"""Tests for validation result dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("execution_result_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionResultBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_result_helpers_delegate_and_bind_dependencies(self):
        captured = {}

        def completed_job_result(job, results, *, completed_at, provenance):
            captured["completed"] = (job, results, completed_at, provenance)
            return {"overall": "pass"}

        def run_target_tasks(tasks, *, exception_result_fn, on_target_complete):
            captured["tasks"] = (tasks, exception_result_fn, on_target_complete)
            return [{"target": "mac"}]

        execution = types.SimpleNamespace(
            validation_result_from_run=lambda *args, **kwargs: {"validation": args, **kwargs},
            validation_error_result=lambda *args, **kwargs: {"error": args, **kwargs},
            unreachable_target_result=lambda target, detail="Host unreachable": {"target": target, "detail": detail},
            target_exception_result=lambda target, exc: {"target": target, "error": str(exc)},
            completed_job_result=completed_job_result,
            sorted_target_results=lambda results: list(reversed(results)),
            run_target_tasks=run_target_tasks,
        )
        bindings = {
            "_execution": execution,
            "now_iso": lambda: "now",
            "normalize_provenance": lambda provenance: {"normalized": provenance},
            "target_exception_result": object(),
        }

        self.assertEqual(
            self.mod.validation_result_from_run(
                bindings,
                "mac",
                {"exit_code": 0},
                log_path=Path("/log"),
                validation="full",
                transport_mode="local",
            )["timeout_secs"],
            3600,
        )
        self.assertEqual(
            self.mod.validation_error_result(
                bindings,
                "mac",
                "detail",
                log_path=Path("/log"),
                transport_mode="local",
            )["transport_mode"],
            "local",
        )
        self.assertEqual(self.mod.unreachable_target_result(bindings, "ubuntu"), {"target": "ubuntu", "detail": "Host unreachable"})
        self.assertEqual(self.mod.target_exception_result(bindings, "mac", RuntimeError("boom")), {"target": "mac", "error": "boom"})
        self.assertEqual(self.mod.completed_job_result(bindings, {"id": "job", "provenance": "p"}, [{"target": "mac"}]), {"overall": "pass"})
        self.assertEqual(captured["completed"][2], "now")
        self.assertEqual(captured["completed"][3], {"normalized": "p"})
        self.assertEqual(self.mod.sorted_target_results(bindings, [1, 2]), [2, 1])
        complete = object()
        self.assertEqual(self.mod.run_target_tasks(bindings, [("mac", lambda: {})], on_target_complete=complete), [{"target": "mac"}])
        self.assertIs(captured["tasks"][1], bindings["target_exception_result"])
        self.assertIs(captured["tasks"][2], complete)


if __name__ == "__main__":
    unittest.main()
