#!/usr/bin/env python3
"""Tests for queue wait/drain dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("queue_wait_drain_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueWaitDrainDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self):
        bindings = {
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

    def test_queue_wait_dependencies_bind_polling_dependencies(self) -> None:
        bindings = self._bindings()

        deps = self.mod.queue_wait_dependencies(bindings)

        self.assertIs(deps["load_job_fn"], bindings["load_job"])
        self.assertIs(deps["load_result_fn"], bindings["load_result"])
        self.assertIs(deps["drain_pending_jobs_fn"], bindings["drain_pending_jobs"])
        self.assertIs(deps["current_runner_info_fn"], bindings["current_runner_info"])
        self.assertIs(deps["sleep_fn"], bindings["time"].sleep)
        self.assertIs(deps["poll_secs"], bindings["WAIT_POLL_SECS"])

    def test_queue_drain_dependencies_bind_runner_loop_dependencies(self) -> None:
        bindings = self._bindings()

        deps = self.mod.queue_drain_dependencies(bindings)

        self.assertIs(deps["root"], bindings["ROOT"])
        self.assertIs(deps["drain_lock_path_fn"], bindings["drain_lock_path"])
        self.assertIs(deps["file_lock_fn"], bindings["file_lock"])
        self.assertIs(deps["lock_busy_error_cls"], bindings["LockBusyError"])
        self.assertIs(deps["write_runner_info_fn"], bindings["write_runner_info"])
        self.assertIs(deps["clear_runner_info_fn"], bindings["clear_runner_info"])
        self.assertIs(deps["reclaim_stale_remote_validators_fn"], bindings["reclaim_stale_remote_validators"])
        self.assertIs(deps["claim_next_job_fn"], bindings["claim_next_job"])
        self.assertIs(deps["process_job_fn"], bindings["process_job"])
        self.assertIs(deps["save_result_fn"], bindings["save_result"])
        self.assertIs(deps["finalize_job_fn"], bindings["finalize_job"])
        self.assertIs(deps["print_result_fn"], bindings["print_result"])
        self.assertIs(deps["now_fn"], bindings["now_iso"])
        self.assertIs(deps["pid_fn"], bindings["os"].getpid)


if __name__ == "__main__":
    unittest.main()
