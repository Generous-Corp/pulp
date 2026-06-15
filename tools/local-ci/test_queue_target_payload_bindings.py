#!/usr/bin/env python3
"""Tests for queue target-state payload facade bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("queue_target_payload_bindings.py")


class QueueTargetPayloadBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_target_payload_exports_match_facade_helpers(self):
        expected = (
            "initial_target_state",
            "completed_target_state",
            "updated_target_state",
            "target_state_snapshot",
        )

        self.assertEqual(self.mod.QUEUE_TARGET_PAYLOAD_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_queue_target_payload_bindings_delegate_to_orchestrator(self):
        captured = {}

        class Orchestrator:
            def initial_target_state(self, *, started_at, log_path):
                captured["initial"] = (started_at, log_path)
                return {"status": "running"}

            def completed_target_state(self, result, previous_state, *, completed_at, default_log_path):
                captured["completed"] = (result, previous_state, completed_at, default_log_path)
                return {"status": "pass"}

            def updated_target_state(self, previous_state, fields):
                captured["updated"] = (previous_state, fields)
                return {"status": "running"}

            def target_state_snapshot(self, target_states):
                captured["snapshot"] = target_states
                return {"mac": {"status": "pass"}}

        bindings = {"_queue_orchestrator": Orchestrator()}

        with mock.patch.object(self.mod, "queue_target_log_path", return_value="/logs/job1-mac.log"):
            self.assertEqual(
                self.mod.initial_target_state(bindings, "job1", "mac", started_at="now"),
                {"status": "running"},
            )
        self.assertEqual(captured["initial"], ("now", "/logs/job1-mac.log"))
        with mock.patch.object(self.mod, "queue_target_log_path", return_value="/logs/job1-mac.log"):
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
        self.assertEqual(self.mod.updated_target_state(bindings, {"status": "pending"}, {"status": "running"}), {"status": "running"})
        self.assertEqual(captured["updated"], ({"status": "pending"}, {"status": "running"}))
        self.assertEqual(self.mod.target_state_snapshot(bindings, {"mac": {"status": "pass"}}), {"mac": {"status": "pass"}})
        self.assertEqual(captured["snapshot"], {"mac": {"status": "pass"}})


if __name__ == "__main__":
    unittest.main()
