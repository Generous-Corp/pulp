#!/usr/bin/env python3
"""Tests for queue wait/drain facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_wait_drain_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueWaitDrainBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, lifecycle=None):
        bindings = {
            "_queue_lifecycle": lifecycle or types.SimpleNamespace(),
            "ROOT": Path("/repo"),
            "WAIT_POLL_SECS": 0.25,
            "os": types.SimpleNamespace(getpid=object()),
            "time": types.SimpleNamespace(sleep=object()),
        }
        for name in [
            "drain_lock_path",
            "file_lock",
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
