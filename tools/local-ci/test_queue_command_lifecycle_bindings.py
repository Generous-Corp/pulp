#!/usr/bin/env python3
"""Tests for queue command lifecycle facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_command_lifecycle_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueCommandLifecycleBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_command_lifecycle_exports_match_facade_helpers(self):
        expected = (
            *self.mod.QUEUE_TERMINAL_RESULT_EXPORTS,
            *self.mod.QUEUE_COMMAND_MUTATION_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_COMMAND_LIFECYCLE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_queue_command_lifecycle_helpers_routes_focused_groups(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_terminal_result_helpers") as install_terminal,
            mock.patch.object(self.mod, "install_queue_command_mutation_helpers") as install_command,
        ):
            self.mod.install_queue_command_lifecycle_helpers(
                bindings,
                ("cancel_job_unlocked", "bump_queue_command_job"),
            )

        install_terminal.assert_called_once_with(bindings, ("cancel_job_unlocked",))
        install_command.assert_called_once_with(bindings, ("bump_queue_command_job",))

    def test_install_queue_command_lifecycle_helpers_preserves_unknown_fallbacks(self):
        bindings = {"custom": object()}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_command_lifecycle_helpers(bindings, ("custom",))

        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
