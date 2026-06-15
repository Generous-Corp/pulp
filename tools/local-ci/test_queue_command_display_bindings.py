#!/usr/bin/env python3
"""Tests for queue command display facade bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("queue_command_display_bindings.py")


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

    def test_install_queue_command_display_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_command_display_helpers(bindings, ("summarize_job", "custom_command_display"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("summarize_job",)),
                mock.call(bindings, self.mod.__dict__, ("custom_command_display",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
