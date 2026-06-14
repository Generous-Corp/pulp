#!/usr/bin/env python3
"""Tests for queue target-state update dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("queue_target_update_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueTargetUpdateDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_target_update_dependencies_bind_locked_queue_dependencies(self) -> None:
        captured = {}

        def update_job_target_state_unlocked(queue, job_id, target_name, fields, *, now_iso_fn):
            captured["target_unlocked"] = (queue, job_id, target_name, fields, now_iso_fn)

        bindings = {
            "_queue_orchestrator": types.SimpleNamespace(update_job_target_state_unlocked=update_job_target_state_unlocked),
            "queue_lock_path": object(),
            "file_lock": object(),
            "load_queue_unlocked": object(),
            "save_queue_unlocked": object(),
            "now_iso": object(),
        }

        deps = self.mod.queue_target_update_dependencies(bindings)

        self.assertIs(deps["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(deps["file_lock_fn"], bindings["file_lock"])
        self.assertIs(deps["load_queue_unlocked_fn"], bindings["load_queue_unlocked"])
        self.assertIs(deps["save_queue_unlocked_fn"], bindings["save_queue_unlocked"])
        deps["update_job_target_state_unlocked_fn"]([], "job1", "mac", {"status": "fail"})
        self.assertEqual(captured["target_unlocked"], ([], "job1", "mac", {"status": "fail"}, bindings["now_iso"]))


if __name__ == "__main__":
    unittest.main()
