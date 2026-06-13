#!/usr/bin/env python3
"""Tests for queue claim/finalize facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_claim_finalize_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueClaimFinalizeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, lifecycle=None, orchestrator=None):
        bindings = {
            "_queue_lifecycle": lifecycle or types.SimpleNamespace(),
            "_queue_orchestrator": orchestrator or types.SimpleNamespace(),
            "ROOT": Path("/repo"),
            "KEEP_COMPLETED_JOBS": 17,
            "os": types.SimpleNamespace(getpid=object()),
        }
        for name in [
            "queue_lock_path",
            "file_lock",
            "load_queue_unlocked",
            "reconcile_running_jobs_unlocked",
            "save_queue_unlocked",
            "normalize_job",
            "trim_completed_jobs_with_removed_ids",
            "collect_local_ci_cleanup_plan",
            "apply_local_ci_cleanup_plan",
            "now_iso",
        ]:
            bindings[name] = object()
        return bindings

    def test_claim_next_job_binds_queue_and_runner_dependencies(self):
        captured = {}

        def claim_next_job_locked(**kwargs):
            captured["claim"] = kwargs
            return {"id": "claimed"}

        def claim_next_job_unlocked(queue, *, runner, now_iso_fn):
            captured["claim_unlocked"] = (queue, runner, now_iso_fn)
            return {"id": "claimed"}

        lifecycle = types.SimpleNamespace(claim_next_job_locked=claim_next_job_locked)
        orchestrator = types.SimpleNamespace(claim_next_job_unlocked=claim_next_job_unlocked)
        bindings = self._bindings(lifecycle=lifecycle, orchestrator=orchestrator)

        self.assertEqual(self.mod.claim_next_job(bindings), {"id": "claimed"})
        self.assertIs(captured["claim"]["root"], bindings["ROOT"])
        self.assertIs(captured["claim"]["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(captured["claim"]["reconcile_running_jobs_unlocked_fn"], bindings["reconcile_running_jobs_unlocked"])
        self.assertIs(captured["claim"]["pid_fn"], bindings["os"].getpid)
        self.assertEqual(captured["claim"]["claim_next_job_unlocked_fn"]([], runner={"pid": 1}), {"id": "claimed"})
        self.assertEqual(captured["claim_unlocked"], ([], {"pid": 1}, bindings["now_iso"]))

    def test_finalize_job_binds_completion_and_cleanup_dependencies(self):
        captured = {}

        def finalize_job_locked(*args, **kwargs):
            captured["finalize"] = (args, kwargs)

        def complete_job_unlocked(queue, job_id, result, result_path, *, now_iso_fn):
            captured["complete"] = (queue, job_id, result, result_path, now_iso_fn)

        lifecycle = types.SimpleNamespace(finalize_job_locked=finalize_job_locked)
        orchestrator = types.SimpleNamespace(complete_job_unlocked=complete_job_unlocked)
        bindings = self._bindings(lifecycle=lifecycle, orchestrator=orchestrator)

        self.mod.finalize_job(bindings, "job1", {"overall": "pass"}, Path("/result.json"))

        self.assertEqual(captured["finalize"][0], ("job1", {"overall": "pass"}, Path("/result.json")))
        self.assertIs(captured["finalize"][1]["trim_completed_jobs_with_removed_ids_fn"], bindings["trim_completed_jobs_with_removed_ids"])
        self.assertIs(captured["finalize"][1]["collect_local_ci_cleanup_plan_fn"], bindings["collect_local_ci_cleanup_plan"])
        self.assertEqual(captured["finalize"][1]["keep_results"], 17)
        self.assertEqual(captured["finalize"][1]["keep_bundles"], 0)
        self.assertFalse(captured["finalize"][1]["include_prepared"])

        captured["finalize"][1]["complete_job_unlocked_fn"]([], "job1", {"overall": "pass"}, Path("/result.json"))
        self.assertEqual(captured["complete"], ([], "job1", {"overall": "pass"}, Path("/result.json"), bindings["now_iso"]))


if __name__ == "__main__":
    unittest.main()
