#!/usr/bin/env python3
"""Tests for stale running queue reconciliation bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_stale_reconcile_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStaleReconcileBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_stale_reconcile_exports_match_wrappers(self):
        expected = ("reconcile_running_jobs_unlocked",)

        self.assertEqual(self.mod.QUEUE_STALE_RECONCILE_EXPORTS, expected)
        self.assertTrue(callable(self.mod.reconcile_running_jobs_unlocked))

    def test_reconcile_running_jobs_binds_stale_requeue_dependencies(self):
        captured = {}

        def reconcile_running_jobs_unlocked(queue, **kwargs):
            captured["reconcile"] = (queue, kwargs)
            return queue, True

        def requeue(job, *, now_iso_fn):
            captured["requeue"] = (job, now_iso_fn)

        lifecycle = types.SimpleNamespace(reconcile_running_jobs_unlocked=reconcile_running_jobs_unlocked)
        orchestrator = types.SimpleNamespace(
            stale_running_reconciliation_actions_unlocked=object(),
            requeue_stale_running_job_unlocked=requeue,
        )
        bindings = {
            "_queue_lifecycle": lifecycle,
            "_queue_orchestrator": orchestrator,
            "stale_running_jobs_unlocked": object(),
            "supersede_job_unlocked": object(),
            "now_iso": object(),
        }

        queue = [{"id": "job1"}]
        self.assertEqual(self.mod.reconcile_running_jobs_unlocked(bindings, queue), (queue, True))
        self.assertIs(captured["reconcile"][1]["stale_running_jobs_unlocked_fn"], bindings["stale_running_jobs_unlocked"])
        self.assertIs(captured["reconcile"][1]["supersede_job_unlocked_fn"], bindings["supersede_job_unlocked"])
        self.assertIs(
            captured["reconcile"][1]["stale_running_reconciliation_actions_unlocked_fn"],
            orchestrator.stale_running_reconciliation_actions_unlocked,
        )
        captured["reconcile"][1]["requeue_stale_running_job_unlocked_fn"]({"id": "old"})
        self.assertEqual(captured["requeue"], ({"id": "old"}, bindings["now_iso"]))


if __name__ == "__main__":
    unittest.main()
