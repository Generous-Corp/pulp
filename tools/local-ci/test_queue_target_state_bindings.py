#!/usr/bin/env python3
"""Tests for queue target-state facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_target_state_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueTargetStateBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_target_state_exports_match_facade_helpers(self):
        expected = (
            *self.mod.QUEUE_TARGET_PAYLOAD_EXPORTS,
            *self.mod.QUEUE_ACTIVE_TARGET_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_TARGET_STATE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_queue_target_state_helpers_wires_named_exports(self):
        bindings = {
            "_queue_orchestrator": types.SimpleNamespace(
                updated_target_state=lambda previous, fields: {"previous": previous, **fields},
                target_state_snapshot=lambda states: {"snapshot": states},
            ),
        }

        self.mod.install_queue_target_state_helpers(bindings, ("updated_target_state", "target_state_snapshot"))

        self.assertEqual(
            bindings["updated_target_state"]({"status": "pending"}, {"status": "running"}),
            {"previous": {"status": "pending"}, "status": "running"},
        )
        self.assertEqual(bindings["target_state_snapshot"]({"mac": {}}), {"snapshot": {"mac": {}}})

    def test_install_queue_target_state_helpers_routes_focused_exports(self):
        captured = {}

        def upsert_job_active_targets_unlocked(queue, job_id, active_targets, *, now_iso_fn):
            captured["upsert"] = (queue, job_id, active_targets, now_iso_fn)
            return True

        bindings = {
            "_queue_orchestrator": types.SimpleNamespace(
                updated_target_state=lambda previous, fields: {"previous": previous, **fields},
                upsert_job_active_targets_unlocked=upsert_job_active_targets_unlocked,
            ),
            "now_iso": object(),
        }

        self.mod.install_queue_target_state_helpers(bindings, ("updated_target_state", "upsert_job_active_targets_unlocked"))

        self.assertEqual(
            bindings["updated_target_state"]({"status": "pending"}, {"status": "running"}),
            {"previous": {"status": "pending"}, "status": "running"},
        )
        self.assertTrue(bindings["upsert_job_active_targets_unlocked"]([], "job1", None))
        self.assertIs(captured["upsert"][3], bindings["now_iso"])

    def test_install_queue_target_state_helpers_preserves_unknown_fallback(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_target_state_helpers(bindings, ("unknown_helper",))

        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
