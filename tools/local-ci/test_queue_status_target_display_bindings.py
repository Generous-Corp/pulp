#!/usr/bin/env python3
"""Tests for queue target-status display bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("queue_status_target_display_bindings.py")


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

    def test_install_queue_status_target_display_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_status_target_display_helpers(
                bindings,
                ("status_target_states", "custom_target_status"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("status_target_states",)),
                mock.call(bindings, self.mod.__dict__, ("custom_target_status",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
