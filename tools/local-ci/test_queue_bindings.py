#!/usr/bin/env python3
"""Tests for queue facade composition."""

from module_test_utils import load_module_from_path
from unittest import mock
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.QUEUE_LIFECYCLE_EXPORTS,
            *self.mod.QUEUE_POLICY_EXPORTS,
            *self.mod.QUEUE_DISPLAY_EXPORTS,
            *self.mod.QUEUE_TARGET_STATE_EXPORTS,
            *self.mod.QUEUE_RUNNER_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_queue_helpers_routes_each_group(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_lifecycle_helpers") as install_lifecycle,
            mock.patch.object(self.mod, "install_queue_policy_helpers") as install_policy,
            mock.patch.object(self.mod, "install_queue_display_helpers") as install_display,
            mock.patch.object(self.mod, "install_queue_target_state_helpers") as install_target_state,
            mock.patch.object(self.mod, "install_queue_runner_helpers") as install_runner,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_queue_helpers(
                bindings,
                (
                    "load_queue",
                    "default_priority_for",
                    "summarize_job",
                    "updated_target_state",
                    "read_runner_info",
                    "custom_queue_export",
                ),
            )

        install_lifecycle.assert_called_once_with(bindings, ("load_queue",))
        install_policy.assert_called_once_with(bindings, ("default_priority_for",))
        install_display.assert_called_once_with(bindings, ("summarize_job",))
        install_target_state.assert_called_once_with(bindings, ("updated_target_state",))
        install_runner.assert_called_once_with(bindings, ("read_runner_info",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_queue_export",))


if __name__ == "__main__":
    unittest.main()
