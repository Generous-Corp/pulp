#!/usr/bin/env python3
"""Tests for stale running queue reconciliation bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("queue_stale_reconcile_bindings.py")


class QueueStaleReconcileBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_stale_reconcile_exports_match_wrappers(self):
        expected = ("reconcile_running_jobs_unlocked",)

        self.assertEqual(self.mod.QUEUE_STALE_RECONCILE_EXPORTS, expected)
        self.assertTrue(callable(self.mod.reconcile_running_jobs_unlocked))

    def test_reconcile_running_jobs_delegates_with_assembled_dependencies(self):
        captured = {}

        def reconcile_running_jobs_unlocked(queue, **kwargs):
            captured["reconcile"] = (queue, kwargs)
            return queue, True

        lifecycle = types.SimpleNamespace(reconcile_running_jobs_unlocked=reconcile_running_jobs_unlocked)
        bindings = {
            "_queue_lifecycle": lifecycle,
            "stale_running_jobs_unlocked": object(),
            "supersede_job_unlocked": object(),
            "now_iso": object(),
        }
        deps = {"stale_running_jobs_unlocked_fn": object(), "supersede_job_unlocked_fn": object()}

        queue = [{"id": "job1"}]
        with mock.patch.object(self.mod, "queue_stale_reconcile_dependencies", return_value=deps):
            self.assertEqual(self.mod.reconcile_running_jobs_unlocked(bindings, queue), (queue, True))
        self.assertIs(captured["reconcile"][1]["stale_running_jobs_unlocked_fn"], deps["stale_running_jobs_unlocked_fn"])
        self.assertIs(captured["reconcile"][1]["supersede_job_unlocked_fn"], deps["supersede_job_unlocked_fn"])

    def test_install_queue_stale_reconcile_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_stale_reconcile_helpers(bindings, ("reconcile_running_jobs_unlocked", "custom_reconcile"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("reconcile_running_jobs_unlocked",)),
                mock.call(bindings, self.mod.__dict__, ("custom_reconcile",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
