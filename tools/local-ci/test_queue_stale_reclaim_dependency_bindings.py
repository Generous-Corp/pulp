#!/usr/bin/env python3
"""Tests for stale remote validator reclaim dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("queue_stale_reclaim_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStaleReclaimDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_stale_reclaim_dependencies_bind_cleanup_dependencies(self) -> None:
        cleanup = types.SimpleNamespace(reclaim_stale_remote_validator_candidates=object())
        bindings = {
            "_cleanup": cleanup,
            "queue_lock_path": object(),
            "file_lock": object(),
            "load_queue_unlocked": object(),
            "collect_stale_windows_cleanup_candidates_unlocked": object(),
            "save_queue_unlocked": object(),
            "cleanup_stale_windows_validator": object(),
            "update_job_target_state": object(),
            "now_iso": object(),
            "trim_line": object(),
        }

        deps = self.mod.queue_stale_reclaim_dependencies(bindings)

        self.assertIs(deps["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(deps["file_lock_fn"], bindings["file_lock"])
        self.assertIs(deps["load_queue_unlocked_fn"], bindings["load_queue_unlocked"])
        self.assertIs(
            deps["collect_stale_windows_cleanup_candidates_unlocked_fn"],
            bindings["collect_stale_windows_cleanup_candidates_unlocked"],
        )
        self.assertIs(deps["save_queue_unlocked_fn"], bindings["save_queue_unlocked"])
        self.assertIs(deps["cleanup_validator_fn"], bindings["cleanup_stale_windows_validator"])
        self.assertIs(deps["update_job_target_state_fn"], bindings["update_job_target_state"])
        self.assertIs(deps["now_fn"], bindings["now_iso"])
        self.assertIs(deps["trim_line_fn"], bindings["trim_line"])
        self.assertIs(deps["reclaim_stale_remote_validator_candidates_fn"], cleanup.reclaim_stale_remote_validator_candidates)


if __name__ == "__main__":
    unittest.main()
