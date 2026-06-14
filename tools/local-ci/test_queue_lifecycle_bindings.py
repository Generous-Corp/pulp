#!/usr/bin/env python3
"""Tests for locked queue lifecycle facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("queue_lifecycle_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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
        calls = []

        def load_install(bindings, names):
            calls.append(("load", names))

        def enqueue_install(bindings, names):
            calls.append(("enqueue", names))

        def command_install(bindings, names):
            calls.append(("command", names))

        def state_install(bindings, names):
            calls.append(("state", names))

        def drain_install(bindings, names):
            calls.append(("drain", names))

        def local_install(bindings, globals_obj, names):
            calls.append(("local", names))

        self.mod.install_queue_load_helpers = load_install
        self.mod.install_queue_enqueue_helpers = enqueue_install
        self.mod.install_queue_command_lifecycle_helpers = command_install
        self.mod.install_queue_state_lifecycle_helpers = state_install
        self.mod.install_queue_drain_helpers = drain_install
        self.mod.install_local_helpers = local_install

        self.mod.install_queue_lifecycle_helpers(
            {},
            (
                "load_queue",
                "enqueue_job",
                "cancel_queue_command_job",
                "update_job_target_state",
                "wait_for_job",
                "custom_queue_lifecycle_export",
            ),
        )

        self.assertEqual(
            calls,
            [
                ("load", ("load_queue",)),
                ("enqueue", ("enqueue_job",)),
                ("command", ("cancel_queue_command_job",)),
                ("state", ("update_job_target_state",)),
                ("drain", ("wait_for_job",)),
                ("local", ("custom_queue_lifecycle_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
