#!/usr/bin/env python3
"""Tests for queue active-target and load facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest

from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_active_load_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueActiveLoadBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, lifecycle=None):
        bindings = {"_queue_lifecycle": lifecycle or types.SimpleNamespace()}
        for name in [
            "queue_lock_path",
            "file_lock",
            "load_queue_unlocked",
            "save_queue_unlocked",
            "upsert_job_active_targets_unlocked",
            "reconcile_running_jobs_unlocked",
            "find_job_unlocked",
            "normalize_job",
        ]:
            bindings[name] = object()
        return bindings

    def test_active_targets_and_load_job_bind_locked_queue_dependencies(self):
        captured = {}

        def update_job_active_targets_locked(*args, **kwargs):
            captured["active"] = (args, kwargs)

        def load_job_locked(*args, **kwargs):
            captured["load_job"] = (args, kwargs)
            return {"id": "job1"}

        lifecycle = types.SimpleNamespace(
            update_job_active_targets_locked=update_job_active_targets_locked,
            load_job_locked=load_job_locked,
        )
        bindings = self._bindings(lifecycle=lifecycle)

        self.mod.update_job_active_targets(bindings, "job1", {"mac": {"status": "running"}})
        self.assertEqual(captured["active"][0], ("job1", {"mac": {"status": "running"}}))
        self.assertIs(captured["active"][1]["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(captured["active"][1]["upsert_job_active_targets_unlocked_fn"], bindings["upsert_job_active_targets_unlocked"])

        self.assertEqual(self.mod.load_job(bindings, "job1"), {"id": "job1"})
        self.assertEqual(captured["load_job"][0], ("job1",))
        self.assertIs(captured["load_job"][1]["reconcile_running_jobs_unlocked_fn"], bindings["reconcile_running_jobs_unlocked"])
        self.assertIs(captured["load_job"][1]["find_job_unlocked_fn"], bindings["find_job_unlocked"])


if __name__ == "__main__":
    unittest.main()
