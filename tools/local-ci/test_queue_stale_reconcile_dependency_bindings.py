#!/usr/bin/env python3
"""Tests for stale running queue reconciliation dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("queue_stale_reconcile_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStaleReconcileDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_stale_reconcile_dependencies_bind_stale_requeue_dependencies(self) -> None:
        captured = {}

        def requeue(job, *, now_iso_fn):
            captured["requeue"] = (job, now_iso_fn)

        orchestrator = types.SimpleNamespace(
            stale_running_reconciliation_actions_unlocked=object(),
            requeue_stale_running_job_unlocked=requeue,
        )
        bindings = {
            "_queue_orchestrator": orchestrator,
            "stale_running_jobs_unlocked": object(),
            "supersede_job_unlocked": object(),
            "now_iso": object(),
        }

        deps = self.mod.queue_stale_reconcile_dependencies(bindings)

        self.assertIs(deps["stale_running_jobs_unlocked_fn"], bindings["stale_running_jobs_unlocked"])
        self.assertIs(deps["supersede_job_unlocked_fn"], bindings["supersede_job_unlocked"])
        self.assertIs(
            deps["stale_running_reconciliation_actions_unlocked_fn"],
            orchestrator.stale_running_reconciliation_actions_unlocked,
        )
        deps["requeue_stale_running_job_unlocked_fn"]({"id": "old"})
        self.assertEqual(captured["requeue"], ({"id": "old"}, bindings["now_iso"]))


if __name__ == "__main__":
    unittest.main()
