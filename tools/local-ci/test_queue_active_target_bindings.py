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

    def test_install_queue_active_target_helpers_wires_named_exports(self):
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

        self.mod.install_queue_active_target_helpers(bindings)

        self.assertTrue(bindings["upsert_job_active_targets_unlocked"]([], "job1", None))
        self.assertEqual(captured["upsert"][1:3], ("job1", None))
        self.assertIs(captured["upsert"][3], bindings["now_iso"])

    def test_install_queue_active_target_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_queue_active_target_helper = lambda _bindings: "future"

        self.mod.install_queue_active_target_helpers(bindings, ("future_queue_active_target_helper",))

        self.assertEqual(bindings["future_queue_active_target_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
