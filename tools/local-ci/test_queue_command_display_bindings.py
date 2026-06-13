#!/usr/bin/env python3
"""Tests for queue command display facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_command_display_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueCommandDisplayBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_command_display_exports_match_facade_helpers(self):
        expected = (
            "summarize_job",
            "bump_queue_command_result_line",
            "cancel_queue_command_result_line",
            "enqueue_command_result_line",
            "drain_runner_active_line",
        )

        self.assertEqual(self.mod.QUEUE_COMMAND_DISPLAY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_command_display_bindings_delegate_to_orchestrator(self):
        calls = []

        def record(name, value):
            def wrapper(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return wrapper

        orchestrator = types.SimpleNamespace(
            summarize_job=record("summarize_job", "summary"),
            bump_queue_command_result_line=record("bump_queue_command_result_line", (0, "bumped")),
            cancel_queue_command_result_line=record("cancel_queue_command_result_line", (0, "canceled")),
            enqueue_command_result_line=record("enqueue_command_result_line", "enqueued"),
            drain_runner_active_line=record("drain_runner_active_line", "active"),
        )
        bindings = {"_queue_orchestrator": orchestrator}

        self.assertEqual(self.mod.summarize_job(bindings, {"id": "job"}), "summary")
        self.assertEqual(self.mod.bump_queue_command_result_line(bindings, {"status": "bumped"}, "job"), (0, "bumped"))
        self.assertEqual(self.mod.cancel_queue_command_result_line(bindings, {"status": "canceled"}, "job"), (0, "canceled"))
        self.assertEqual(self.mod.enqueue_command_result_line(bindings, {"id": "job"}, created=True), "enqueued")
        self.assertEqual(self.mod.drain_runner_active_line(bindings, {"pid": 1}), "active")

        self.assertEqual(calls[0], ("summarize_job", ({"id": "job"},), {}))
        self.assertEqual(calls[3], ("enqueue_command_result_line", ({"id": "job"},), {"created": True}))


if __name__ == "__main__":
    unittest.main()
