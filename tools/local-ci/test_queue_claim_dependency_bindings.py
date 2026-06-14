#!/usr/bin/env python3
"""Tests for queue claim dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
from pathlib import Path
import types
import unittest



def load_module():
    return load_local_ci_module("queue_claim_dependency_bindings.py")


class QueueClaimDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, orchestrator=None):
        bindings = {
            "_queue_orchestrator": orchestrator or types.SimpleNamespace(),
            "ROOT": Path("/repo"),
            "os": types.SimpleNamespace(getpid=object()),
        }
        for name in [
            "queue_lock_path",
            "file_lock",
            "load_queue_unlocked",
            "reconcile_running_jobs_unlocked",
            "save_queue_unlocked",
            "normalize_job",
            "now_iso",
        ]:
            bindings[name] = object()
        return bindings

    def test_queue_claim_dependencies_bind_queue_and_runner_dependencies(self) -> None:
        captured = {}

        def claim_next_job_unlocked(queue, *, runner, now_iso_fn):
            captured["claim_unlocked"] = (queue, runner, now_iso_fn)
            return {"id": "claimed"}

        orchestrator = types.SimpleNamespace(claim_next_job_unlocked=claim_next_job_unlocked)
        bindings = self._bindings(orchestrator=orchestrator)

        deps = self.mod.queue_claim_dependencies(bindings)

        self.assertIs(deps["root"], bindings["ROOT"])
        self.assertIs(deps["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(deps["file_lock_fn"], bindings["file_lock"])
        self.assertIs(deps["load_queue_unlocked_fn"], bindings["load_queue_unlocked"])
        self.assertIs(deps["reconcile_running_jobs_unlocked_fn"], bindings["reconcile_running_jobs_unlocked"])
        self.assertIs(deps["save_queue_unlocked_fn"], bindings["save_queue_unlocked"])
        self.assertIs(deps["normalize_job_fn"], bindings["normalize_job"])
        self.assertIs(deps["pid_fn"], bindings["os"].getpid)
        self.assertEqual(deps["claim_next_job_unlocked_fn"]([], runner={"pid": 1}), {"id": "claimed"})
        self.assertEqual(captured["claim_unlocked"], ([], {"pid": 1}, bindings["now_iso"]))


if __name__ == "__main__":
    unittest.main()
