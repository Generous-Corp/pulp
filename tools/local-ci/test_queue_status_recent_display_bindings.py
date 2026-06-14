#!/usr/bin/env python3
"""Tests for recent completed queue status display bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_status_recent_display_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStatusRecentDisplayBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_recent_status_exports_match_wrappers(self):
        expected = (
            "recent_completed_status_line",
            "recent_completed_missing_result_line",
        )

        self.assertEqual(self.mod.QUEUE_STATUS_RECENT_DISPLAY_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_recent_status_bindings_delegate_to_orchestrator(self):
        calls = []

        def record(name, value):
            def wrapper(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return wrapper

        orchestrator = types.SimpleNamespace(
            recent_completed_status_line=record("recent_completed_status_line", "recent"),
            recent_completed_missing_result_line=record("recent_completed_missing_result_line", "missing result"),
        )
        bindings = {"_queue_orchestrator": orchestrator}

        self.assertEqual(self.mod.recent_completed_status_line(bindings, {"id": "job"}, {"overall": "pass"}), "recent")
        self.assertEqual(self.mod.recent_completed_missing_result_line(bindings, {"id": "job"}), "missing result")
        self.assertEqual(
            calls,
            [
                ("recent_completed_status_line", ({"id": "job"}, {"overall": "pass"}), {}),
                ("recent_completed_missing_result_line", ({"id": "job"},), {}),
            ],
        )


if __name__ == "__main__":
    unittest.main()
