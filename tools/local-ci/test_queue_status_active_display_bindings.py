#!/usr/bin/env python3
"""Tests for active queue status display bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_status_active_display_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStatusActiveDisplayBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_active_status_exports_match_wrappers(self):
        expected = (
            "summarize_active_targets",
            "status_active_targets",
            "status_runner_line",
        )

        self.assertEqual(self.mod.QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_active_status_bindings_delegate_to_orchestrator(self):
        calls = []

        def record(name, value):
            def wrapper(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return wrapper

        orchestrator = types.SimpleNamespace(
            summarize_active_targets=record("summarize_active_targets", "targets"),
            status_active_targets=record("status_active_targets", {"mac": {}}),
            status_runner_line=record("status_runner_line", "runner"),
        )
        bindings = {"_queue_orchestrator": orchestrator}

        self.assertEqual(self.mod.summarize_active_targets(bindings, {"mac": {}}, ["mac"]), "targets")
        self.assertEqual(self.mod.status_active_targets(bindings, {"id": "job"}, {"mac": {}}), {"mac": {}})
        self.assertEqual(self.mod.status_runner_line(bindings, {"pid": 1}), "runner")
        self.assertEqual(
            calls,
            [
                ("summarize_active_targets", ({"mac": {}}, ["mac"]), {}),
                ("status_active_targets", ({"id": "job"}, {"mac": {}}), {}),
                ("status_runner_line", ({"pid": 1},), {}),
            ],
        )


if __name__ == "__main__":
    unittest.main()
