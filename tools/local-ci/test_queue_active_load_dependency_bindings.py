#!/usr/bin/env python3
"""Tests for queue active-target and load dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("queue_active_load_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueActiveLoadDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self):
        bindings = {}
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

    def test_queue_active_target_dependencies_bind_update_dependencies(self) -> None:
        bindings = self._bindings()

        deps = self.mod.queue_active_target_dependencies(bindings)

        self.assertIs(deps["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(deps["file_lock_fn"], bindings["file_lock"])
        self.assertIs(deps["load_queue_unlocked_fn"], bindings["load_queue_unlocked"])
        self.assertIs(deps["upsert_job_active_targets_unlocked_fn"], bindings["upsert_job_active_targets_unlocked"])
        self.assertIs(deps["save_queue_unlocked_fn"], bindings["save_queue_unlocked"])

    def test_queue_load_job_dependencies_bind_load_dependencies(self) -> None:
        bindings = self._bindings()

        deps = self.mod.queue_load_job_dependencies(bindings)

        self.assertIs(deps["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(deps["file_lock_fn"], bindings["file_lock"])
        self.assertIs(deps["load_queue_unlocked_fn"], bindings["load_queue_unlocked"])
        self.assertIs(deps["reconcile_running_jobs_unlocked_fn"], bindings["reconcile_running_jobs_unlocked"])
        self.assertIs(deps["save_queue_unlocked_fn"], bindings["save_queue_unlocked"])
        self.assertIs(deps["find_job_unlocked_fn"], bindings["find_job_unlocked"])
        self.assertIs(deps["normalize_job_fn"], bindings["normalize_job"])


if __name__ == "__main__":
    unittest.main()
