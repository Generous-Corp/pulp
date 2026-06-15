#!/usr/bin/env python3
"""Tests for locked queue enqueue dependency assembly."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("queue_enqueue_dependency_bindings.py")


class QueueEnqueueDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, orchestrator=None):
        bindings = {
            "_queue_orchestrator": orchestrator or types.SimpleNamespace(),
            "ROOT": Path("/repo"),
        }
        for name in [
            "queue_lock_path",
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
        ]:
            bindings[name] = object()
        return bindings

    def test_queue_enqueue_dependencies_bind_lifecycle_dependencies_and_now_lambda(self) -> None:
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
        bindings = self._bindings(orchestrator=orchestrator)

        deps = self.mod.queue_enqueue_dependencies(bindings)

        self.assertIs(deps["make_job_fn"], bindings["make_job"])
        self.assertIs(deps["supersede_job_unlocked_fn"], bindings["supersede_job_unlocked"])
        self.assertIs(deps["find_active_job_by_fingerprint_unlocked_fn"], orchestrator.find_active_job_by_fingerprint_unlocked)
        self.assertIs(deps["pending_supersedence_candidates_unlocked_fn"], orchestrator.pending_supersedence_candidates_unlocked)
        self.assertTrue(deps["bump_pending_job_priority_unlocked_fn"]({"id": "old"}, "high"))
        self.assertEqual(bumped["job"], {"id": "old"})
        self.assertEqual(bumped["priority"], "high")
        self.assertIs(bumped["now"], bindings["now_iso"])


if __name__ == "__main__":
    unittest.main()
