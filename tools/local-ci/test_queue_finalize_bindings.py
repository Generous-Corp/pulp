#!/usr/bin/env python3
"""Tests for queue finalize facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("queue_finalize_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueFinalizeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, lifecycle=None, orchestrator=None):
        bindings = {
            "_queue_lifecycle": lifecycle or types.SimpleNamespace(),
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

    def test_finalize_exports_match_facade_helpers(self) -> None:
        self.assertEqual(self.mod.QUEUE_FINALIZE_EXPORTS, ("finalize_job",))

    def test_finalize_job_binds_completion_and_cleanup_dependencies(self) -> None:
        captured = {}

        def finalize_job_locked(*args, **kwargs):
            captured["finalize"] = (args, kwargs)

        def complete_job_unlocked(queue, job_id, result, result_path, *, now_iso_fn):
            captured["complete"] = (queue, job_id, result, result_path, now_iso_fn)

        lifecycle = types.SimpleNamespace(finalize_job_locked=finalize_job_locked)
        orchestrator = types.SimpleNamespace(complete_job_unlocked=complete_job_unlocked)
        bindings = self._bindings(lifecycle=lifecycle, orchestrator=orchestrator)
        result = {"overall": "pass"}
        result_path = Path("/result.json")

        self.mod.finalize_job(bindings, "job1", result, result_path)

        self.assertEqual(captured["finalize"][0], ("job1", result, result_path))
        kwargs = captured["finalize"][1]
        self.assertIs(kwargs["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(kwargs["file_lock_fn"], bindings["file_lock"])
        self.assertIs(kwargs["load_queue_unlocked_fn"], bindings["load_queue_unlocked"])
        self.assertIs(kwargs["trim_completed_jobs_with_removed_ids_fn"], bindings["trim_completed_jobs_with_removed_ids"])
        self.assertIs(kwargs["save_queue_unlocked_fn"], bindings["save_queue_unlocked"])
        self.assertIs(kwargs["collect_local_ci_cleanup_plan_fn"], bindings["collect_local_ci_cleanup_plan"])
        self.assertIs(kwargs["apply_local_ci_cleanup_plan_fn"], bindings["apply_local_ci_cleanup_plan"])
        self.assertEqual(kwargs["keep_results"], 17)
        self.assertEqual(kwargs["keep_logs"], 17)
        self.assertEqual(kwargs["keep_bundles"], 0)
        self.assertFalse(kwargs["include_prepared"])

        kwargs["complete_job_unlocked_fn"]([], "job1", result, result_path)
        self.assertEqual(captured["complete"], ([], "job1", result, result_path, bindings["now_iso"]))

    def test_install_queue_finalize_helpers_wires_named_export(self) -> None:
        bindings = self._bindings()

        self.mod.install_queue_finalize_helpers(bindings, ("finalize_job",))

        self.assertIn("finalize_job", bindings)
        self.assertEqual(bindings["finalize_job"].__name__, "finalize_job")

    def test_install_queue_finalize_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_queue_finalize_helper = lambda _bindings: "future"

        self.mod.install_queue_finalize_helpers(bindings, ("future_queue_finalize_helper",))

        self.assertEqual(bindings["future_queue_finalize_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
