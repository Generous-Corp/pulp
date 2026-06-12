#!/usr/bin/env python3
"""Tests for locked queue lifecycle facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_lifecycle_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueLifecycleBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, lifecycle=None, orchestrator=None):
        bindings = {
            "_queue_lifecycle": lifecycle or types.SimpleNamespace(),
            "_queue_orchestrator": orchestrator or types.SimpleNamespace(),
            "_cleanup": types.SimpleNamespace(),
            "ROOT": Path("/repo"),
            "KEEP_COMPLETED_JOBS": 17,
            "WAIT_POLL_SECS": 0.25,
            "os": types.SimpleNamespace(getpid=object()),
            "time": types.SimpleNamespace(sleep=object()),
        }
        for name in [
            "queue_lock_path",
            "drain_lock_path",
            "file_lock",
            "load_queue_unlocked",
            "save_queue_unlocked",
            "reconcile_running_jobs_unlocked",
            "stale_running_jobs_unlocked",
            "normalize_priority",
            "normalize_validation_mode",
            "make_fingerprint",
            "make_job",
            "supersede_job_unlocked",
            "trim_completed_jobs",
            "normalize_job",
            "now_iso",
            "summarize_job",
            "cancel_job_unlocked",
            "upsert_job_active_targets_unlocked",
            "find_job_unlocked",
            "trim_completed_jobs_with_removed_ids",
            "collect_local_ci_cleanup_plan",
            "apply_local_ci_cleanup_plan",
            "load_job",
            "load_result",
            "drain_pending_jobs",
            "current_runner_info",
            "write_runner_info",
            "clear_runner_info",
            "reclaim_stale_remote_validators",
            "claim_next_job",
            "process_job",
            "save_result",
            "finalize_job",
            "print_result",
            "LockBusyError",
            "supersedence_result",
            "cancellation_result",
            "collect_stale_windows_cleanup_candidates_unlocked",
            "cleanup_stale_windows_validator",
            "update_job_target_state",
            "trim_line",
        ]:
            bindings[name] = object()
        return bindings

    def test_enqueue_job_binds_lifecycle_dependencies_and_now_lambda(self):
        captured = {}

        def enqueue(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"id": "job"}, True

        bumped = {}

        def bump_pending(job, priority, *, now_iso_fn):
            bumped["job"] = job
            bumped["priority"] = priority
            bumped["now"] = now_iso_fn
            return True

        orchestrator = types.SimpleNamespace(
            find_active_job_by_fingerprint_unlocked=object(),
            bump_pending_job_priority_unlocked=bump_pending,
            pending_supersedence_candidates_unlocked=object(),
        )
        bindings = self._bindings(
            lifecycle=types.SimpleNamespace(enqueue_job_locked=enqueue),
            orchestrator=orchestrator,
        )

        result = self.mod.enqueue_job(
            bindings,
            "feature/topic",
            "abc123",
            "normal",
            ["mac"],
            "local",
            "full",
            submission={"source": "test"},
        )

        self.assertEqual(result, ({"id": "job"}, True))
        self.assertEqual(captured["args"], ("feature/topic", "abc123", "normal", ["mac"], "local", "full"))
        self.assertEqual(captured["kwargs"]["submission"], {"source": "test"})
        self.assertIs(captured["kwargs"]["make_job_fn"], bindings["make_job"])
        self.assertIs(captured["kwargs"]["supersede_job_unlocked_fn"], bindings["supersede_job_unlocked"])
        self.assertIs(captured["kwargs"]["find_active_job_by_fingerprint_unlocked_fn"], orchestrator.find_active_job_by_fingerprint_unlocked)

        self.assertTrue(captured["kwargs"]["bump_pending_job_priority_unlocked_fn"]({"id": "old"}, "high"))
        self.assertEqual(bumped["job"], {"id": "old"})
        self.assertEqual(bumped["priority"], "high")
        self.assertIs(bumped["now"], bindings["now_iso"])

    def test_claim_finalize_wait_and_drain_bind_dependencies(self):
        captured = {}

        def claim_next_job_locked(**kwargs):
            captured["claim"] = kwargs
            return {"id": "claimed"}

        def finalize_job_locked(*args, **kwargs):
            captured["finalize"] = (args, kwargs)

        def wait_for_job_completion(*args, **kwargs):
            captured["wait"] = (args, kwargs)
            return {"overall": "pass"}, 0

        def drain_pending_jobs_locked(*args, **kwargs):
            captured["drain"] = (args, kwargs)
            return True, False

        def claim_next_job_unlocked(queue, *, runner, now_iso_fn):
            captured["claim_unlocked"] = (queue, runner, now_iso_fn)
            return {"id": "claimed"}

        def complete_job_unlocked(queue, job_id, result, result_path, *, now_iso_fn):
            captured["complete"] = (queue, job_id, result, result_path, now_iso_fn)

        lifecycle = types.SimpleNamespace(
            claim_next_job_locked=claim_next_job_locked,
            finalize_job_locked=finalize_job_locked,
            wait_for_job_completion=wait_for_job_completion,
            drain_pending_jobs_locked=drain_pending_jobs_locked,
        )
        orchestrator = types.SimpleNamespace(
            claim_next_job_unlocked=claim_next_job_unlocked,
            complete_job_unlocked=complete_job_unlocked,
        )
        bindings = self._bindings(lifecycle=lifecycle, orchestrator=orchestrator)

        self.assertEqual(self.mod.claim_next_job(bindings), {"id": "claimed"})
        self.assertIs(captured["claim"]["root"], bindings["ROOT"])
        self.assertIs(captured["claim"]["pid_fn"], bindings["os"].getpid)
        self.assertEqual(
            captured["claim"]["claim_next_job_unlocked_fn"]([], runner={"pid": 1}),
            {"id": "claimed"},
        )
        self.assertIs(captured["claim_unlocked"][2], bindings["now_iso"])

        self.mod.finalize_job(bindings, "job1", {"overall": "pass"}, Path("/result.json"))
        self.assertEqual(captured["finalize"][0], ("job1", {"overall": "pass"}, Path("/result.json")))
        self.assertEqual(captured["finalize"][1]["keep_results"], 17)
        captured["finalize"][1]["complete_job_unlocked_fn"]([], "job1", {"overall": "pass"}, Path("/result.json"))
        self.assertIs(captured["complete"][4], bindings["now_iso"])

        self.assertEqual(self.mod.wait_for_job(bindings, "job1", {"targets": {}}), ({"overall": "pass"}, 0))
        self.assertEqual(captured["wait"][0], ("job1", {"targets": {}}))
        self.assertIs(captured["wait"][1]["sleep_fn"], bindings["time"].sleep)
        self.assertIs(captured["wait"][1]["poll_secs"], bindings["WAIT_POLL_SECS"])

        self.assertEqual(self.mod.drain_pending_jobs(bindings, {"defaults": {}}, blocking=False), (True, False))
        self.assertEqual(captured["drain"][0], ({"defaults": {}},))
        self.assertFalse(captured["drain"][1]["blocking"])
        self.assertIs(captured["drain"][1]["root"], bindings["ROOT"])
        self.assertIs(captured["drain"][1]["pid_fn"], bindings["os"].getpid)

    def test_reconcile_target_state_and_stale_validator_bindings(self):
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

        def reclaim_stale_remote_validators_locked(**kwargs):
            captured["reclaim"] = kwargs
            return 2

        lifecycle = types.SimpleNamespace(
            reconcile_running_jobs_unlocked=reconcile_running_jobs_unlocked,
            update_job_target_state_locked=update_job_target_state_locked,
            reclaim_stale_remote_validators_locked=reclaim_stale_remote_validators_locked,
        )
        orchestrator = types.SimpleNamespace(
            stale_running_reconciliation_actions_unlocked=object(),
            requeue_stale_running_job_unlocked=requeue,
            update_job_target_state_unlocked=update_job_target_state_unlocked,
        )
        cleanup = types.SimpleNamespace(reclaim_stale_remote_validator_candidates=object())
        bindings = self._bindings(lifecycle=lifecycle, orchestrator=orchestrator)
        bindings["_cleanup"] = cleanup

        queue = [{"id": "job1"}]
        self.assertEqual(self.mod.reconcile_running_jobs_unlocked(bindings, queue), (queue, True))
        self.assertIs(captured["reconcile"][1]["stale_running_jobs_unlocked_fn"], bindings["stale_running_jobs_unlocked"])
        self.assertIs(captured["reconcile"][1]["supersede_job_unlocked_fn"], bindings["supersede_job_unlocked"])
        captured["reconcile"][1]["requeue_stale_running_job_unlocked_fn"]({"id": "stale"})
        self.assertEqual(captured["requeue"], ({"id": "stale"}, bindings["now_iso"]))

        self.mod.update_job_target_state(bindings, "job1", "mac", status="running")
        self.assertEqual(captured["target_state"][0], ("job1", "mac", {"status": "running"}))
        captured["target_state"][1]["update_job_target_state_unlocked_fn"]([], "job1", "mac", {"status": "pass"})
        self.assertEqual(captured["target_unlocked"][4], bindings["now_iso"])

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
