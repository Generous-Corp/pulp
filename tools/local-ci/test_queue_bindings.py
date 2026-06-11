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
            "_cleanup": types.SimpleNamespace(),
            "ROOT": Path("/repo"),
            "KEEP_COMPLETED_JOBS": 17,
            "WAIT_POLL_SECS": 0.25,
            "os": types.SimpleNamespace(getpid=object()),
            "time": types.SimpleNamespace(sleep=object()),
            "uuid": types.SimpleNamespace(uuid4=lambda: types.SimpleNamespace(hex="abcdef1234567890")),
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
            "target_log_path",
            "trim_line",
            "validate_ci_branch_name",
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

    def test_queue_policy_display_and_state_bindings_delegate_to_orchestrator(self):
        captured = {}

        def make_job(*args, **kwargs):
            captured["make_job"] = (args, kwargs)
            return {"id": "job1"}

        def supersedence_result(job, superseded_by, reason, *, now_iso_fn):
            captured["supersedence_result"] = (job, superseded_by, reason, now_iso_fn)
            return {"overall": "superseded"}

        def cancellation_result(job, reason, *, now_iso_fn):
            captured["cancellation_result"] = (job, reason, now_iso_fn)
            return {"overall": "canceled"}

        def initial_target_state(*, started_at, log_path):
            captured["initial_target_state"] = (started_at, log_path)
            return {"status": "running"}

        def completed_target_state(result, previous_state, *, completed_at, default_log_path):
            captured["completed_target_state"] = (result, previous_state, completed_at, default_log_path)
            return {"status": "pass"}

        def upsert_job_active_targets_unlocked(queue, job_id, active_targets, *, now_iso_fn):
            captured["upsert"] = (queue, job_id, active_targets, now_iso_fn)
            return True

        def trim_completed_jobs_with_removed_ids(queue, *, keep_completed_jobs):
            captured["trim_removed"] = (queue, keep_completed_jobs)
            return queue, {"old"}

        def trim_completed_jobs(queue, *, keep_completed_jobs):
            captured["trim"] = (queue, keep_completed_jobs)
            return queue

        orchestrator = types.SimpleNamespace(
            default_priority_for=lambda command, config: f"{command}:{config['priority']}",
            make_fingerprint=lambda branch, sha, targets, validation: "|".join([branch, sha, ",".join(targets), validation]),
            make_job=make_job,
            supersedence_result=supersedence_result,
            cancellation_result=cancellation_result,
            summarize_job=lambda job: f"summary:{job['id']}",
            enqueue_command_result_line=lambda job, *, created: f"{created}:{job['id']}",
            initial_target_state=initial_target_state,
            completed_target_state=completed_target_state,
            upsert_job_active_targets_unlocked=upsert_job_active_targets_unlocked,
            trim_completed_jobs_with_removed_ids=trim_completed_jobs_with_removed_ids,
            trim_completed_jobs=trim_completed_jobs,
            validate_ci_branch_name=lambda branch: branch.strip(),
        )
        bindings = self._bindings(orchestrator=orchestrator)
        bindings["target_log_path"] = lambda job_id, target: Path(f"/logs/{job_id}-{target}.log")

        self.assertEqual(self.mod.default_priority_for(bindings, "ship", {"priority": "high"}), "ship:high")
        self.assertEqual(self.mod.make_fingerprint(bindings, "b", "s", ["mac"], "full"), "b|s|mac|full")
        self.assertEqual(
            self.mod.make_job(bindings, "branch", "sha", "normal", ["mac"], "run", "full"),
            {"id": "job1"},
        )
        self.assertIs(captured["make_job"][1]["now_iso_fn"], bindings["now_iso"])
        self.assertEqual(captured["make_job"][1]["uuid_hex_fn"](), "abcdef1234567890")
        self.assertIs(captured["make_job"][1]["root"], bindings["ROOT"])
        self.assertIs(captured["make_job"][1]["validate_branch_fn"], bindings["validate_ci_branch_name"])
        self.assertEqual(
            self.mod.supersedence_result(bindings, {"id": "old"}, "new", "newer_sha"),
            {"overall": "superseded"},
        )
        self.assertIs(captured["supersedence_result"][3], bindings["now_iso"])
        self.assertEqual(self.mod.cancellation_result(bindings, {"id": "old"}, "operator"), {"overall": "canceled"})
        self.assertIs(captured["cancellation_result"][2], bindings["now_iso"])
        self.assertEqual(self.mod.summarize_job(bindings, {"id": "job1"}), "summary:job1")
        self.assertEqual(self.mod.enqueue_command_result_line(bindings, {"id": "job1"}, created=True), "True:job1")
        self.assertEqual(
            self.mod.initial_target_state(bindings, "job1", "mac", started_at="now"),
            {"status": "running"},
        )
        self.assertEqual(captured["initial_target_state"], ("now", "/logs/job1-mac.log"))
        self.assertEqual(
            self.mod.completed_target_state(
                bindings,
                "job1",
                "mac",
                {"status": "pass"},
                {"status": "running"},
                completed_at="done",
            ),
            {"status": "pass"},
        )
        self.assertEqual(captured["completed_target_state"][3], "/logs/job1-mac.log")
        self.assertTrue(self.mod.upsert_job_active_targets_unlocked(bindings, [], "job1", {"mac": {}}))
        self.assertIs(captured["upsert"][3], bindings["now_iso"])
        self.assertEqual(self.mod.trim_completed_jobs_with_removed_ids(bindings, [{"id": "old"}]), ([{"id": "old"}], {"old"}))
        self.assertEqual(captured["trim_removed"][1], 17)
        self.assertEqual(self.mod.trim_completed_jobs(bindings, [{"id": "old"}]), [{"id": "old"}])
        self.assertEqual(captured["trim"][1], 17)
        self.assertEqual(self.mod.validate_ci_branch_name(bindings, " branch "), "branch")

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

    def test_reconcile_and_target_state_bind_queue_runner_dependencies(self):
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
        self.assertIs(captured["reconcile"][1]["supersede_job_unlocked_fn"], bindings["supersede_job_unlocked"])
        captured["reconcile"][1]["requeue_stale_running_job_unlocked_fn"]({"id": "stale"})
        self.assertEqual(captured["requeue"], ({"id": "stale"}, bindings["now_iso"]))

        self.mod.update_job_target_state(bindings, "job1", "mac", status="running")
        self.assertEqual(captured["target_state"][0], ("job1", "mac", {"status": "running"}))
        captured["target_state"][1]["update_job_target_state_unlocked_fn"]([], "job1", "mac", {"status": "pass"})
        self.assertEqual(captured["target_unlocked"][4], bindings["now_iso"])

    def test_runner_state_facade_bindings_delegate_to_runner_state_module(self):
        calls = []

        runner_state = types.SimpleNamespace(
            read_runner_info=lambda: calls.append("read") or {"pid": 123},
            pid_alive=lambda pid: calls.append(("pid", pid)) or True,
            current_runner_info=lambda: calls.append("current") or {"pid": 456},
            stale_running_jobs_for_current_runner=lambda queue, **kwargs: calls.append(("stale", queue, kwargs)) or [{"id": "old"}],
            write_runner_info=lambda info: calls.append(("write", info)),
            clear_runner_info=lambda: calls.append("clear"),
        )
        orchestrator = types.SimpleNamespace(stale_running_jobs_for_runner_unlocked=object())
        bindings = self._bindings(orchestrator=orchestrator)
        bindings["_runner_state"] = runner_state

        self.assertEqual(self.mod.read_runner_info(bindings), {"pid": 123})
        self.assertTrue(self.mod.pid_alive(bindings, 789))
        self.assertEqual(self.mod.current_runner_info(bindings), {"pid": 456})
        self.assertEqual(self.mod.stale_running_jobs_unlocked(bindings, [{"id": "run"}]), [{"id": "old"}])
        self.mod.write_runner_info(bindings, {"pid": 1})
        self.mod.clear_runner_info(bindings)

        self.assertEqual(calls[0:3], ["read", ("pid", 789), "current"])
        self.assertEqual(calls[3][0], "stale")
        self.assertIs(calls[3][2]["stale_running_jobs_for_runner_unlocked_fn"], orchestrator.stale_running_jobs_for_runner_unlocked)
        self.assertEqual(calls[4:], [("write", {"pid": 1}), "clear"])

    def test_reclaim_stale_remote_validators_binds_cleanup_dependencies(self):
        captured = {}

        def reclaim_stale_remote_validators_locked(**kwargs):
            captured["kwargs"] = kwargs
            return 2

        lifecycle = types.SimpleNamespace(reclaim_stale_remote_validators_locked=reclaim_stale_remote_validators_locked)
        cleanup = types.SimpleNamespace(reclaim_stale_remote_validator_candidates=object())
        bindings = self._bindings(lifecycle=lifecycle)
        bindings["_cleanup"] = cleanup

        self.assertEqual(self.mod.reclaim_stale_remote_validators(bindings, {"targets": {}}), 2)
        self.assertIs(
            captured["kwargs"]["collect_stale_windows_cleanup_candidates_unlocked_fn"],
            bindings["collect_stale_windows_cleanup_candidates_unlocked"],
        )
        self.assertIs(captured["kwargs"]["cleanup_validator_fn"], bindings["cleanup_stale_windows_validator"])
        self.assertIs(captured["kwargs"]["update_job_target_state_fn"], bindings["update_job_target_state"])
        self.assertIs(captured["kwargs"]["now_fn"], bindings["now_iso"])
        self.assertIs(captured["kwargs"]["trim_line_fn"], bindings["trim_line"])
        self.assertIs(captured["kwargs"]["reclaim_stale_remote_validator_candidates_fn"], cleanup.reclaim_stale_remote_validator_candidates)


if __name__ == "__main__":
    unittest.main()
