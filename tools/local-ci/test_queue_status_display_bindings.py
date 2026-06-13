#!/usr/bin/env python3
"""Tests for queue status display facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_status_display_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStatusDisplayBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_status_display_exports_match_facade_helpers(self):
        expected = (
            *self.mod.QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS[:2],
            *self.mod.QUEUE_STATUS_TARGET_DISPLAY_EXPORTS,
            self.mod.QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS[2],
            *self.mod.QUEUE_STATUS_RECENT_DISPLAY_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_STATUS_DISPLAY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_status_display_bindings_delegate_to_orchestrator(self):
        calls = []

        def record(name, value):
            def wrapper(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return wrapper

        orchestrator = types.SimpleNamespace(
            summarize_active_targets=record("summarize_active_targets", "targets"),
            status_active_targets=record("status_active_targets", {"mac": {}}),
            status_target_states=record("status_target_states", [("mac", {})]),
            status_submission_lines=record("status_submission_lines", ["submission"]),
            target_state_detail_parts=record("target_state_detail_parts", ["detail"]),
            status_target_detail_lines=record("status_target_detail_lines", ["target detail"]),
            status_runner_line=record("status_runner_line", "runner"),
            recent_completed_status_line=record("recent_completed_status_line", "recent"),
            recent_completed_missing_result_line=record("recent_completed_missing_result_line", "missing result"),
        )
        bindings = {"_queue_orchestrator": orchestrator}

        self.assertEqual(self.mod.summarize_active_targets(bindings, {"mac": {}}, ["mac"]), "targets")
        self.assertEqual(self.mod.status_active_targets(bindings, {"id": "job"}, {"mac": {}}), {"mac": {}})
        self.assertEqual(self.mod.status_target_states(bindings, {"id": "job"}, {"mac": {}}), [("mac", {})])
        self.assertEqual(self.mod.status_submission_lines(bindings, {"id": "job"}), ["submission"])
        self.assertEqual(self.mod.target_state_detail_parts(bindings, {"status": "pass"}), ["detail"])
        self.assertEqual(self.mod.status_target_detail_lines(bindings, {"id": "job"}, {"mac": {}}), ["target detail"])
        self.assertEqual(self.mod.status_runner_line(bindings, {"pid": 1}), "runner")
        self.assertEqual(self.mod.recent_completed_status_line(bindings, {"id": "job"}, {"overall": "pass"}), "recent")
        self.assertEqual(self.mod.recent_completed_missing_result_line(bindings, {"id": "job"}), "missing result")

        self.assertEqual(calls[0], ("summarize_active_targets", ({"mac": {}}, ["mac"]), {}))


if __name__ == "__main__":
    unittest.main()
