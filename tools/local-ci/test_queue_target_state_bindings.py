#!/usr/bin/env python3
"""Tests for queue target-state facade bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("queue_target_state_bindings.py")


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

    def test_install_queue_target_state_helpers_routes_groups_and_unknown_fallback(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_target_payload_helpers") as payload,
            mock.patch.object(self.mod, "install_queue_active_target_helpers") as active,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_queue_target_state_helpers(
                bindings,
                ("updated_target_state", "upsert_job_active_targets_unlocked", "unknown_helper"),
            )

        payload.assert_called_once_with(bindings, ("updated_target_state",))
        active.assert_called_once_with(bindings, ("upsert_job_active_targets_unlocked",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
