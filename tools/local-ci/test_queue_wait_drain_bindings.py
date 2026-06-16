#!/usr/bin/env python3
"""Tests for queue wait/drain facade bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock
from pathlib import Path



def load_module():
    return load_local_ci_module("queue_wait_drain_bindings.py")


class QueueWaitDrainBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_wait_drain_exports_match_facade_helpers(self):
        expected = (
            "wait_for_job",
            "drain_pending_jobs",
        )

        self.assertEqual(self.mod.QUEUE_WAIT_DRAIN_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_wait_drain_helpers_installs_requested_facades(self):
        bindings = self._bindings()

        self.mod.install_queue_wait_drain_helpers(bindings, ("wait_for_job",))

        self.assertIn("wait_for_job", bindings)
        self.assertIsNot(bindings["drain_pending_jobs"], self.mod.drain_pending_jobs)
        self.assertEqual(bindings["wait_for_job"].__name__, "wait_for_job")

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

    def test_wait_and_drain_delegate_with_assembled_dependencies(self):
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
        wait_deps = {"load_job_fn": object(), "poll_secs": 0.25}
        drain_deps = {"root": object(), "pid_fn": object()}

        with mock.patch.object(self.mod, "queue_wait_dependencies", return_value=wait_deps):
            self.assertEqual(self.mod.wait_for_job(bindings, "job1", {"targets": {}}), ({"overall": "pass"}, 0))

        self.assertEqual(captured["wait"][0], ("job1", {"targets": {}}))
        self.assertIs(captured["wait"][1]["load_job_fn"], wait_deps["load_job_fn"])
        self.assertIs(captured["wait"][1]["poll_secs"], wait_deps["poll_secs"])

        with mock.patch.object(self.mod, "queue_drain_dependencies", return_value=drain_deps):
            self.assertEqual(self.mod.drain_pending_jobs(bindings, {"defaults": {}}, blocking=False), (True, False))

        self.assertEqual(captured["drain"][0], ({"defaults": {}},))
        self.assertFalse(captured["drain"][1]["blocking"])
        self.assertIs(captured["drain"][1]["root"], drain_deps["root"])
        self.assertIs(captured["drain"][1]["pid_fn"], drain_deps["pid_fn"])


if __name__ == "__main__":
    unittest.main()
