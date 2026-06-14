#!/usr/bin/env python3
"""Tests for queue finalize dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("queue_finalize_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueFinalizeDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, orchestrator=None):
        bindings = {
            "_queue_orchestrator": orchestrator or types.SimpleNamespace(),
            "KEEP_COMPLETED_JOBS": 17,
        }
        for name in [
            "queue_lock_path",
            "file_lock",
            "load_queue_unlocked",
            "trim_completed_jobs_with_removed_ids",
            "save_queue_unlocked",
            "collect_local_ci_cleanup_plan",
            "apply_local_ci_cleanup_plan",
            "now_iso",
        ]:
            bindings[name] = object()
        return bindings

    def test_queue_finalize_dependencies_bind_completion_and_cleanup_dependencies(self) -> None:
        captured = {}

        def complete_job_unlocked(queue, job_id, result, result_path, *, now_iso_fn):
            captured["complete"] = (queue, job_id, result, result_path, now_iso_fn)

        orchestrator = types.SimpleNamespace(complete_job_unlocked=complete_job_unlocked)
        bindings = self._bindings(orchestrator=orchestrator)
        result = {"overall": "pass"}
        result_path = Path("/result.json")

        deps = self.mod.queue_finalize_dependencies(bindings)

        self.assertIs(deps["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(deps["file_lock_fn"], bindings["file_lock"])
        self.assertIs(deps["load_queue_unlocked_fn"], bindings["load_queue_unlocked"])
        self.assertIs(deps["trim_completed_jobs_with_removed_ids_fn"], bindings["trim_completed_jobs_with_removed_ids"])
        self.assertIs(deps["save_queue_unlocked_fn"], bindings["save_queue_unlocked"])
        self.assertIs(deps["collect_local_ci_cleanup_plan_fn"], bindings["collect_local_ci_cleanup_plan"])
        self.assertIs(deps["apply_local_ci_cleanup_plan_fn"], bindings["apply_local_ci_cleanup_plan"])
        self.assertEqual(deps["keep_results"], 17)
        self.assertEqual(deps["keep_logs"], 17)
        self.assertEqual(deps["keep_bundles"], 0)
        self.assertFalse(deps["include_prepared"])

        deps["complete_job_unlocked_fn"]([], "job1", result, result_path)
        self.assertEqual(captured["complete"], ([], "job1", result, result_path, bindings["now_iso"]))


if __name__ == "__main__":
    unittest.main()
