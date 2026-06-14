#!/usr/bin/env python3
"""Tests for queue drain and runner lifecycle facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_drain_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueDrainBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_drain_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.QUEUE_CLAIM_EXPORTS,
            *self.mod.QUEUE_FINALIZE_EXPORTS,
            *self.mod.QUEUE_WAIT_DRAIN_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_DRAIN_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_drain_helpers_routes_focused_groups(self):
        calls = []

        def claim_install(bindings, names):
            calls.append(("claim", names))

        def finalize_install(bindings, names):
            calls.append(("finalize", names))

        def wait_install(bindings, names):
            calls.append(("wait", names))

        self.mod.install_queue_claim_helpers = claim_install
        self.mod.install_queue_finalize_helpers = finalize_install
        self.mod.install_queue_wait_drain_helpers = wait_install

        self.mod.install_queue_drain_helpers({}, ("claim_next_job", "finalize_job", "wait_for_job"))

        self.assertEqual(
            calls,
            [
                ("claim", ("claim_next_job",)),
                ("finalize", ("finalize_job",)),
                ("wait", ("wait_for_job",)),
            ],
        )

    def _bindings(self, lifecycle=None, orchestrator=None):
        bindings = {
            "_queue_lifecycle": lifecycle or types.SimpleNamespace(),
            "_queue_orchestrator": orchestrator or types.SimpleNamespace(),
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
            "reconcile_running_jobs_unlocked",
            "save_queue_unlocked",
            "normalize_job",
            "trim_completed_jobs_with_removed_ids",
            "collect_local_ci_cleanup_plan",
            "apply_local_ci_cleanup_plan",
            "load_job",
            "load_result",
            "drain_pending_jobs",
            "current_runner_info",
            "LockBusyError",
            "write_runner_info",
            "clear_runner_info",
            "reclaim_stale_remote_validators",
            "claim_next_job",
            "process_job",
            "save_result",
            "finalize_job",
            "print_result",
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
        self.assertEqual(
            captured["claim"]["claim_next_job_unlocked_fn"]([], runner={"pid": 1}),
            {"id": "claimed"},
        )
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

    def test_wait_and_drain_bind_runner_loop_dependencies(self):
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
        self.assertIs(captured["wait"][1]["load_job_fn"], bindings["load_job"])
        self.assertIs(captured["wait"][1]["sleep_fn"], bindings["time"].sleep)
        self.assertIs(captured["wait"][1]["poll_secs"], bindings["WAIT_POLL_SECS"])

        self.assertEqual(self.mod.drain_pending_jobs(bindings, {"defaults": {}}, blocking=False), (True, False))
        self.assertEqual(captured["drain"][0], ({"defaults": {}},))
        self.assertFalse(captured["drain"][1]["blocking"])
        self.assertIs(captured["drain"][1]["root"], bindings["ROOT"])
        self.assertIs(captured["drain"][1]["lock_busy_error_cls"], bindings["LockBusyError"])
        self.assertIs(captured["drain"][1]["claim_next_job_fn"], bindings["claim_next_job"])
        self.assertIs(captured["drain"][1]["pid_fn"], bindings["os"].getpid)


if __name__ == "__main__":
    unittest.main()
