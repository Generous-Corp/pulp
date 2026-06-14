#!/usr/bin/env python3
"""Tests for locked queue enqueue dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock
from pathlib import Path



def load_module():
    return load_local_ci_module("queue_enqueue_bindings.py")


class QueueEnqueueBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_enqueue_exports_match_wrappers(self):
        expected = ("enqueue_job",)

        self.assertEqual(self.mod.QUEUE_ENQUEUE_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def _bindings(self, lifecycle=None, orchestrator=None):
        bindings = {
            "_queue_lifecycle": lifecycle or types.SimpleNamespace(),
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

    def test_enqueue_job_delegates_with_assembled_dependencies(self):
        captured = {}

        def enqueue(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"id": "job"}, True

        bindings = self._bindings(
            lifecycle=types.SimpleNamespace(enqueue_job_locked=enqueue),
        )
        deps = {"make_job_fn": object(), "supersede_job_unlocked_fn": object()}

        with mock.patch.object(self.mod, "queue_enqueue_dependencies", return_value=deps):
            result = self.mod.enqueue_job(
                bindings,
                "feature/topic",
                "abc123",
                "normal",
                ["mac"],
                "local",
                "full",
                submission={"source": "test"},
            )

        self.assertEqual(result, ({"id": "job"}, True))
        self.assertEqual(captured["args"], ("feature/topic", "abc123", "normal", ["mac"], "local", "full"))
        self.assertEqual(captured["kwargs"]["submission"], {"source": "test"})
        self.assertIs(captured["kwargs"]["make_job_fn"], deps["make_job_fn"])
        self.assertIs(captured["kwargs"]["supersede_job_unlocked_fn"], deps["supersede_job_unlocked_fn"])


if __name__ == "__main__":
    unittest.main()
