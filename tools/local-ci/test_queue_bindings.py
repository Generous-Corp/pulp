#!/usr/bin/env python3
"""Tests for queue facade composition."""

from module_test_utils import load_module_from_path
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
        calls = []

        def lifecycle_install(bindings, names):
            calls.append(("lifecycle", names))

        def policy_install(bindings, names):
            calls.append(("policy", names))

        def display_install(bindings, names):
            calls.append(("display", names))

        def target_state_install(bindings, names):
            calls.append(("target_state", names))

        def runner_install(bindings, names):
            calls.append(("runner", names))

        def local_install(bindings, globals_obj, names):
            calls.append(("local", names))

        self.mod.install_queue_lifecycle_helpers = lifecycle_install
        self.mod.install_queue_policy_helpers = policy_install
        self.mod.install_queue_display_helpers = display_install
        self.mod.install_queue_target_state_helpers = target_state_install
        self.mod.install_queue_runner_helpers = runner_install
        self.mod.install_local_helpers = local_install

        self.mod.install_queue_helpers(
            {},
            (
                "load_queue",
                "default_priority_for",
                "summarize_job",
                "updated_target_state",
                "read_runner_info",
                "custom_queue_export",
            ),
        )

        self.assertEqual(
            calls,
            [
                ("lifecycle", ("load_queue",)),
                ("policy", ("default_priority_for",)),
                ("display", ("summarize_job",)),
                ("target_state", ("updated_target_state",)),
                ("runner", ("read_runner_info",)),
                ("local", ("custom_queue_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
