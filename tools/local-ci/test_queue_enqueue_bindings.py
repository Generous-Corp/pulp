#!/usr/bin/env python3
"""Tests for locked queue enqueue dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_enqueue_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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

    def test_enqueue_job_binds_lifecycle_dependencies_and_now_lambda(self):
        captured = {}

        def enqueue(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"id": "job"}, True

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
        bindings = self._bindings(
            lifecycle=types.SimpleNamespace(enqueue_job_locked=enqueue),
            orchestrator=orchestrator,
        )

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
        self.assertIs(captured["kwargs"]["make_job_fn"], bindings["make_job"])
        self.assertIs(captured["kwargs"]["supersede_job_unlocked_fn"], bindings["supersede_job_unlocked"])
        self.assertIs(captured["kwargs"]["find_active_job_by_fingerprint_unlocked_fn"], orchestrator.find_active_job_by_fingerprint_unlocked)

        self.assertTrue(captured["kwargs"]["bump_pending_job_priority_unlocked_fn"]({"id": "old"}, "high"))
        self.assertEqual(bumped["job"], {"id": "old"})
        self.assertEqual(bumped["priority"], "high")
        self.assertIs(bumped["now"], bindings["now_iso"])


if __name__ == "__main__":
    unittest.main()
