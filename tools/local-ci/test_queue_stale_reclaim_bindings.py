#!/usr/bin/env python3
"""Tests for stale remote validator reclaim bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_stale_reclaim_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStaleReclaimBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_stale_reclaim_exports_match_wrappers(self):
        expected = ("reclaim_stale_remote_validators",)

        self.assertEqual(self.mod.QUEUE_STALE_RECLAIM_EXPORTS, expected)
        self.assertTrue(callable(self.mod.reclaim_stale_remote_validators))

    def test_reclaim_stale_remote_validators_binds_cleanup_dependencies(self):
        captured = {}

        def reclaim_stale_remote_validators_locked(**kwargs):
            captured["reclaim"] = kwargs
            return 2

        lifecycle = types.SimpleNamespace(reclaim_stale_remote_validators_locked=reclaim_stale_remote_validators_locked)
        cleanup = types.SimpleNamespace(reclaim_stale_remote_validator_candidates=object())
        bindings = {
            "_queue_lifecycle": lifecycle,
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

        self.assertEqual(self.mod.reclaim_stale_remote_validators(bindings, {"targets": {}}), 2)
        self.assertIs(captured["reclaim"]["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(captured["reclaim"]["file_lock_fn"], bindings["file_lock"])
        self.assertIs(captured["reclaim"]["load_queue_unlocked_fn"], bindings["load_queue_unlocked"])
        self.assertIs(
            captured["reclaim"]["collect_stale_windows_cleanup_candidates_unlocked_fn"],
            bindings["collect_stale_windows_cleanup_candidates_unlocked"],
        )
        self.assertIs(captured["reclaim"]["save_queue_unlocked_fn"], bindings["save_queue_unlocked"])
        self.assertIs(captured["reclaim"]["cleanup_validator_fn"], bindings["cleanup_stale_windows_validator"])
        self.assertIs(captured["reclaim"]["update_job_target_state_fn"], bindings["update_job_target_state"])
        self.assertIs(captured["reclaim"]["now_fn"], bindings["now_iso"])
        self.assertIs(captured["reclaim"]["trim_line_fn"], bindings["trim_line"])
        self.assertIs(captured["reclaim"]["reclaim_stale_remote_validator_candidates_fn"], cleanup.reclaim_stale_remote_validator_candidates)


if __name__ == "__main__":
    unittest.main()
