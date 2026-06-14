#!/usr/bin/env python3
"""Tests for queue command mutation dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("queue_command_mutation_dependency_bindings.py")


class QueueCommandMutationDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, orchestrator=None):
        bindings = {
            "_queue_orchestrator": orchestrator or types.SimpleNamespace(),
        }
        for name in [
            "queue_lock_path",
            "file_lock",
            "load_queue_unlocked",
            "save_queue_unlocked",
            "cancel_job_unlocked",
            "trim_completed_jobs",
            "summarize_job",
            "now_iso",
        ]:
            bindings[name] = object()
        return bindings

    def test_queue_bump_command_dependencies_bind_lock_and_now_dependencies(self) -> None:
        captured = {}

        def set_pending_job_priority_unlocked(job, priority, *, now_iso_fn):
            captured["set_priority"] = (job, priority, now_iso_fn)
            return True

        orchestrator = types.SimpleNamespace(
            find_queue_command_job_unlocked=object(),
            set_pending_job_priority_unlocked=set_pending_job_priority_unlocked,
        )
        bindings = self._bindings(orchestrator=orchestrator)

        deps = self.mod.queue_bump_command_dependencies(bindings)

        self.assertIs(deps["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(deps["file_lock_fn"], bindings["file_lock"])
        self.assertIs(deps["load_queue_unlocked_fn"], bindings["load_queue_unlocked"])
        self.assertIs(deps["find_queue_command_job_unlocked_fn"], orchestrator.find_queue_command_job_unlocked)
        self.assertTrue(deps["set_pending_job_priority_unlocked_fn"]({"id": "job1"}, "high"))
        self.assertEqual(captured["set_priority"], ({"id": "job1"}, "high", bindings["now_iso"]))
        self.assertIs(deps["save_queue_unlocked_fn"], bindings["save_queue_unlocked"])
        self.assertIs(deps["summarize_job_fn"], bindings["summarize_job"])

    def test_queue_cancel_command_dependencies_bind_command_dependencies(self) -> None:
        orchestrator = types.SimpleNamespace(find_queue_command_job_unlocked=object())
        bindings = self._bindings(orchestrator=orchestrator)

        deps = self.mod.queue_cancel_command_dependencies(bindings)

        self.assertIs(deps["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(deps["file_lock_fn"], bindings["file_lock"])
        self.assertIs(deps["load_queue_unlocked_fn"], bindings["load_queue_unlocked"])
        self.assertIs(deps["find_queue_command_job_unlocked_fn"], orchestrator.find_queue_command_job_unlocked)
        self.assertIs(deps["cancel_job_unlocked_fn"], bindings["cancel_job_unlocked"])
        self.assertIs(deps["trim_completed_jobs_fn"], bindings["trim_completed_jobs"])
        self.assertIs(deps["save_queue_unlocked_fn"], bindings["save_queue_unlocked"])
        self.assertIs(deps["summarize_job_fn"], bindings["summarize_job"])


if __name__ == "__main__":
    unittest.main()
