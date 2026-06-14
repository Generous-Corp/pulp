#!/usr/bin/env python3
"""Tests for queue target-state facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


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

    def test_queue_target_state_bindings_delegate_to_orchestrator(self):
        captured = {}

        class Orchestrator:
            def initial_target_state(self, *, started_at, log_path):
                captured["initial"] = (started_at, log_path)
                return {"status": "running"}

            def completed_target_state(self, result, previous_state, *, completed_at, default_log_path):
                captured["completed"] = (result, previous_state, completed_at, default_log_path)
                return {"status": "pass"}

            def upsert_job_active_targets_unlocked(self, queue, job_id, active_targets, *, now_iso_fn):
                captured["upsert"] = (queue, job_id, active_targets, now_iso_fn)
                return True

            def updated_target_state(self, previous_state, fields):
                captured["updated"] = (previous_state, fields)
                return {"status": "running"}

            def target_state_snapshot(self, target_states):
                captured["snapshot"] = target_states
                return {"mac": {"status": "pass"}}

        bindings = {
            "_queue_orchestrator": Orchestrator(),
            "target_log_path": lambda job_id, target: Path(f"/logs/{job_id}-{target}.log"),
            "now_iso": object(),
        }

        self.assertEqual(
            self.mod.initial_target_state(bindings, "job1", "mac", started_at="now"),
            {"status": "running"},
        )
        self.assertEqual(captured["initial"], ("now", "/logs/job1-mac.log"))
        self.assertEqual(
            self.mod.completed_target_state(
                bindings,
                "job1",
                "mac",
                {"status": "pass"},
                {"status": "running"},
                completed_at="done",
            ),
            {"status": "pass"},
        )
        self.assertEqual(captured["completed"][3], "/logs/job1-mac.log")
        self.assertTrue(self.mod.upsert_job_active_targets_unlocked(bindings, [], "job1", {"mac": {}}))
        self.assertIs(captured["upsert"][3], bindings["now_iso"])
        self.assertEqual(self.mod.updated_target_state(bindings, {"status": "pending"}, {"status": "running"}), {"status": "running"})
        self.assertEqual(captured["updated"], ({"status": "pending"}, {"status": "running"}))
        self.assertEqual(self.mod.target_state_snapshot(bindings, {"mac": {"status": "pass"}}), {"mac": {"status": "pass"}})
        self.assertEqual(captured["snapshot"], {"mac": {"status": "pass"}})

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

    def test_install_queue_target_state_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_queue_target_state_helper = lambda _bindings: "future"

        self.mod.install_queue_target_state_helpers(bindings, ("future_queue_target_state_helper",))

        self.assertEqual(bindings["future_queue_target_state_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
