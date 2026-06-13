#!/usr/bin/env python3
"""Tests for queue target-status display bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_status_target_display_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStatusTargetDisplayBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_target_status_exports_match_wrappers(self):
        expected = (
            "status_target_states",
            "status_submission_lines",
            "target_state_detail_parts",
            "status_target_detail_lines",
        )

        self.assertEqual(self.mod.QUEUE_STATUS_TARGET_DISPLAY_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_target_status_bindings_delegate_to_orchestrator(self):
        calls = []

        def record(name, value):
            def wrapper(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return wrapper

        orchestrator = types.SimpleNamespace(
            status_target_states=record("status_target_states", [("mac", {})]),
            status_submission_lines=record("status_submission_lines", ["submission"]),
            target_state_detail_parts=record("target_state_detail_parts", ["detail"]),
            status_target_detail_lines=record("status_target_detail_lines", ["target detail"]),
        )
        bindings = {"_queue_orchestrator": orchestrator}

        self.assertEqual(self.mod.status_target_states(bindings, {"id": "job"}, {"mac": {}}), [("mac", {})])
        self.assertEqual(self.mod.status_submission_lines(bindings, {"id": "job"}), ["submission"])
        self.assertEqual(self.mod.target_state_detail_parts(bindings, {"status": "pass"}), ["detail"])
        self.assertEqual(self.mod.status_target_detail_lines(bindings, {"id": "job"}, {"mac": {}}), ["target detail"])
        self.assertEqual(
            calls,
            [
                ("status_target_states", ({"id": "job"}, {"mac": {}}), {}),
                ("status_submission_lines", ({"id": "job"},), {}),
                ("target_state_detail_parts", ({"status": "pass"},), {}),
                ("status_target_detail_lines", ({"id": "job"}, {"mac": {}}), {}),
            ],
        )


if __name__ == "__main__":
    unittest.main()
