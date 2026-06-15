#!/usr/bin/env python3
"""Tests for locked queue lifecycle facade bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("queue_lifecycle_bindings.py")


class QueueLifecycleBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_lifecycle_exports_match_facade_helpers(self):
        expected = (
            *self.mod.QUEUE_COMMAND_LIFECYCLE_EXPORTS[:2],
            *self.mod.QUEUE_LOAD_EXPORTS,
            self.mod.QUEUE_STATE_LIFECYCLE_EXPORTS[0],
            *self.mod.QUEUE_ENQUEUE_EXPORTS,
            *self.mod.QUEUE_COMMAND_LIFECYCLE_EXPORTS[2:],
            *self.mod.QUEUE_STATE_LIFECYCLE_EXPORTS[1:],
            *self.mod.QUEUE_DRAIN_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_LIFECYCLE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_queue_lifecycle_helpers_routes_focused_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_load_helpers") as load,
            mock.patch.object(self.mod, "install_queue_enqueue_helpers") as enqueue,
            mock.patch.object(self.mod, "install_queue_command_lifecycle_helpers") as command,
            mock.patch.object(self.mod, "install_queue_state_lifecycle_helpers") as state,
            mock.patch.object(self.mod, "install_queue_drain_helpers") as drain,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_queue_lifecycle_helpers(
                bindings,
                (
                    "load_queue",
                    "enqueue_job",
                    "cancel_queue_command_job",
                    "update_job_target_state",
                    "wait_for_job",
                    "custom_queue_lifecycle_export",
                ),
            )

        load.assert_called_once_with(bindings, ("load_queue",))
        enqueue.assert_called_once_with(bindings, ("enqueue_job",))
        command.assert_called_once_with(bindings, ("cancel_queue_command_job",))
        state.assert_called_once_with(bindings, ("update_job_target_state",))
        drain.assert_called_once_with(bindings, ("wait_for_job",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_queue_lifecycle_export",))


if __name__ == "__main__":
    unittest.main()
