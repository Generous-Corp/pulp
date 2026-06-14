#!/usr/bin/env python3
"""Tests for active-target queue mutation facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("queue_active_target_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueActiveTargetBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_active_target_exports_match_facade_helpers(self):
        expected = ("upsert_job_active_targets_unlocked",)

        self.assertEqual(self.mod.QUEUE_ACTIVE_TARGET_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_upsert_job_active_targets_unlocked_binds_now_iso(self):
        captured = {}

        def upsert_job_active_targets_unlocked(queue, job_id, active_targets, *, now_iso_fn):
            captured["upsert"] = (queue, job_id, active_targets, now_iso_fn)
            return True

        bindings = {
            "_queue_orchestrator": types.SimpleNamespace(
                upsert_job_active_targets_unlocked=upsert_job_active_targets_unlocked,
            ),
            "now_iso": object(),
        }

        self.assertTrue(self.mod.upsert_job_active_targets_unlocked(bindings, [], "job1", {"mac": {}}))
        self.assertIs(captured["upsert"][3], bindings["now_iso"])


if __name__ == "__main__":
    unittest.main()
