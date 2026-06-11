#!/usr/bin/env python3
"""Tests for queue lifecycle facade bindings."""

import importlib.util
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("queue_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class QueueBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, lifecycle=None, orchestrator=None):
        bindings = {
            "_queue_lifecycle": lifecycle or types.SimpleNamespace(),
            "_queue_orchestrator": orchestrator or types.SimpleNamespace(),
            "_runner_state": types.SimpleNamespace(),
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

    def test_active_target_and_runner_updates_bind_facade_dependencies(self):
        calls = {}

        def update_job_active_targets_locked(*args, **kwargs):
            calls["job"] = (args, kwargs)

        def update_current_runner_active_targets(*args, **kwargs):
            calls["runner"] = (args, kwargs)

        def update_runner_info_active_targets(info, job_id, active_targets, *, now_iso_fn):
            calls["runner_mutate"] = (info, job_id, active_targets, now_iso_fn)
            return True

        lifecycle = types.SimpleNamespace(update_job_active_targets_locked=update_job_active_targets_locked)
        runner_state = types.SimpleNamespace(update_current_runner_active_targets=update_current_runner_active_targets)
        orchestrator = types.SimpleNamespace(update_runner_info_active_targets=update_runner_info_active_targets)
        bindings = self._bindings(lifecycle=lifecycle, orchestrator=orchestrator)
        bindings["_runner_state"] = runner_state

        self.mod.update_job_active_targets(bindings, "job1", {"mac": {"status": "running"}})
        self.mod.update_runner_active_targets(bindings, "job1", {"mac": {"status": "pass"}})

        self.assertEqual(calls["job"][0], ("job1", {"mac": {"status": "running"}}))
        self.assertIs(calls["job"][1]["upsert_job_active_targets_unlocked_fn"], bindings["upsert_job_active_targets_unlocked"])
        self.assertEqual(calls["runner"][0], ("job1", {"mac": {"status": "pass"}}))
        update_info = calls["runner"][1]["update_runner_info_active_targets_fn"]
        self.assertTrue(update_info({"pid": 1}, "job1", {"mac": {"status": "pass"}}))
        self.assertIs(calls["runner_mutate"][3], bindings["now_iso"])

    def test_claim_and_finalize_bind_lifecycle_dependencies(self):
        captured = {}

        def claim_next_job_locked(**kwargs):
            captured["claim"] = kwargs
            return {"id": "claimed"}

        def finalize_job_locked(*args, **kwargs):
            captured["finalize"] = (args, kwargs)

        def claim_next_job_unlocked(queue, *, runner, now_iso_fn):
            captured["claim_unlocked"] = (queue, runner, now_iso_fn)
            return {"id": "claimed"}

        def complete_job_unlocked(queue, job_id, result, result_path, *, now_iso_fn):
            captured["complete"] = (queue, job_id, result, result_path, now_iso_fn)

        lifecycle = types.SimpleNamespace(
            claim_next_job_locked=claim_next_job_locked,
            finalize_job_locked=finalize_job_locked,
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

    def test_wait_and_drain_bind_lifecycle_dependencies(self):
        captured = {}

        def wait_for_job_completion(*args, **kwargs):
            captured["wait"] = (args, kwargs)
            return {"overall": "pass"}, 0

        def drain_pending_jobs_locked(*args, **kwargs):
            captured["drain"] = (args, kwargs)
            return True, False

        lifecycle = types.SimpleNamespace(
            wait_for_job_completion=wait_for_job_completion,
            drain_pending_jobs_locked=drain_pending_jobs_locked,
        )
        bindings = self._bindings(lifecycle=lifecycle)

        self.assertEqual(self.mod.wait_for_job(bindings, "job1", {"targets": {}}), ({"overall": "pass"}, 0))
        self.assertEqual(captured["wait"][0], ("job1", {"targets": {}}))
        self.assertIs(captured["wait"][1]["sleep_fn"], bindings["time"].sleep)
        self.assertIs(captured["wait"][1]["poll_secs"], bindings["WAIT_POLL_SECS"])

        self.assertEqual(self.mod.drain_pending_jobs(bindings, {"defaults": {}}, blocking=False), (True, False))
        self.assertEqual(captured["drain"][0], ({"defaults": {}},))
        self.assertFalse(captured["drain"][1]["blocking"])
        self.assertIs(captured["drain"][1]["root"], bindings["ROOT"])
        self.assertIs(captured["drain"][1]["pid_fn"], bindings["os"].getpid)


if __name__ == "__main__":
    unittest.main()
