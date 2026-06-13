#!/usr/bin/env python3
"""Tests for queue state lifecycle facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_state_lifecycle_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStateLifecycleBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_state_lifecycle_exports_match_facade_helpers(self):
        expected = (
            "update_job_active_targets",
            "reconcile_running_jobs_unlocked",
            "update_job_target_state",
            "reclaim_stale_remote_validators",
            "load_job",
        )

        self.assertEqual(self.mod.QUEUE_STATE_LIFECYCLE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def _bindings(self, lifecycle=None, orchestrator=None):
        bindings = {
            "_queue_lifecycle": lifecycle or types.SimpleNamespace(),
            "_queue_orchestrator": orchestrator or types.SimpleNamespace(),
            "_cleanup": types.SimpleNamespace(),
        }
        for name in [
            "queue_lock_path",
            "file_lock",
            "load_queue_unlocked",
            "save_queue_unlocked",
            "upsert_job_active_targets_unlocked",
            "reconcile_running_jobs_unlocked",
            "stale_running_jobs_unlocked",
            "supersede_job_unlocked",
            "collect_stale_windows_cleanup_candidates_unlocked",
            "cleanup_stale_windows_validator",
            "update_job_target_state",
            "find_job_unlocked",
            "normalize_job",
            "trim_line",
            "now_iso",
        ]:
            bindings[name] = object()
        return bindings

    def test_active_targets_and_load_job_bind_locked_queue_dependencies(self):
        captured = {}

        def update_job_active_targets_locked(*args, **kwargs):
            captured["active"] = (args, kwargs)

        def load_job_locked(*args, **kwargs):
            captured["load_job"] = (args, kwargs)
            return {"id": "job1"}

        lifecycle = types.SimpleNamespace(
            update_job_active_targets_locked=update_job_active_targets_locked,
            load_job_locked=load_job_locked,
        )
        bindings = self._bindings(lifecycle=lifecycle)

        self.mod.update_job_active_targets(bindings, "job1", {"mac": {"status": "running"}})
        self.assertEqual(captured["active"][0], ("job1", {"mac": {"status": "running"}}))
        self.assertIs(captured["active"][1]["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(captured["active"][1]["upsert_job_active_targets_unlocked_fn"], bindings["upsert_job_active_targets_unlocked"])

        self.assertEqual(self.mod.load_job(bindings, "job1"), {"id": "job1"})
        self.assertEqual(captured["load_job"][0], ("job1",))
        self.assertIs(captured["load_job"][1]["reconcile_running_jobs_unlocked_fn"], bindings["reconcile_running_jobs_unlocked"])
        self.assertIs(captured["load_job"][1]["find_job_unlocked_fn"], bindings["find_job_unlocked"])

    def test_reconcile_and_target_state_bind_now_lambdas(self):
        captured = {}

        def reconcile_running_jobs_unlocked(queue, **kwargs):
            captured["reconcile"] = (queue, kwargs)
            return queue, True

        def requeue(job, *, now_iso_fn):
            captured["requeue"] = (job, now_iso_fn)

        def update_job_target_state_locked(*args, **kwargs):
            captured["target_state"] = (args, kwargs)

        def update_job_target_state_unlocked(queue, job_id, target_name, fields, *, now_iso_fn):
            captured["target_unlocked"] = (queue, job_id, target_name, fields, now_iso_fn)
            return True

        lifecycle = types.SimpleNamespace(
            reconcile_running_jobs_unlocked=reconcile_running_jobs_unlocked,
            update_job_target_state_locked=update_job_target_state_locked,
        )
        orchestrator = types.SimpleNamespace(
            stale_running_reconciliation_actions_unlocked=object(),
            requeue_stale_running_job_unlocked=requeue,
            update_job_target_state_unlocked=update_job_target_state_unlocked,
        )
        bindings = self._bindings(lifecycle=lifecycle, orchestrator=orchestrator)

        queue = [{"id": "job1"}]
        self.assertEqual(self.mod.reconcile_running_jobs_unlocked(bindings, queue), (queue, True))
        self.assertIs(captured["reconcile"][1]["stale_running_jobs_unlocked_fn"], bindings["stale_running_jobs_unlocked"])
        self.assertIs(
            captured["reconcile"][1]["stale_running_reconciliation_actions_unlocked_fn"],
            orchestrator.stale_running_reconciliation_actions_unlocked,
        )
        captured["reconcile"][1]["requeue_stale_running_job_unlocked_fn"]({"id": "old"})
        self.assertEqual(captured["requeue"], ({"id": "old"}, bindings["now_iso"]))

        self.mod.update_job_target_state(bindings, "job1", "mac", status="pass")
        self.assertEqual(captured["target_state"][0], ("job1", "mac", {"status": "pass"}))
        captured["target_state"][1]["update_job_target_state_unlocked_fn"]([], "job1", "mac", {"status": "fail"})
        self.assertEqual(captured["target_unlocked"], ([], "job1", "mac", {"status": "fail"}, bindings["now_iso"]))

    def test_reclaim_stale_remote_validators_binds_cleanup_dependencies(self):
        captured = {}

        def reclaim_stale_remote_validators_locked(**kwargs):
            captured["reclaim"] = kwargs
            return 2

        lifecycle = types.SimpleNamespace(reclaim_stale_remote_validators_locked=reclaim_stale_remote_validators_locked)
        cleanup = types.SimpleNamespace(reclaim_stale_remote_validator_candidates=object())
        bindings = self._bindings(lifecycle=lifecycle)
        bindings["_cleanup"] = cleanup

        self.assertEqual(self.mod.reclaim_stale_remote_validators(bindings, {"targets": {}}), 2)
        self.assertIs(
            captured["reclaim"]["collect_stale_windows_cleanup_candidates_unlocked_fn"],
            bindings["collect_stale_windows_cleanup_candidates_unlocked"],
        )
        self.assertIs(captured["reclaim"]["cleanup_validator_fn"], bindings["cleanup_stale_windows_validator"])
        self.assertIs(captured["reclaim"]["update_job_target_state_fn"], bindings["update_job_target_state"])
        self.assertIs(captured["reclaim"]["now_fn"], bindings["now_iso"])
        self.assertIs(captured["reclaim"]["trim_line_fn"], bindings["trim_line"])
        self.assertIs(captured["reclaim"]["reclaim_stale_remote_validator_candidates_fn"], cleanup.reclaim_stale_remote_validator_candidates)


if __name__ == "__main__":
    unittest.main()
