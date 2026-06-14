#!/usr/bin/env python3
"""Tests for queue display facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_display_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueDisplayBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_display_exports_match_focused_facade_helpers(self):
        expected = (
            *self.mod.QUEUE_COMMAND_DISPLAY_EXPORTS,
            *self.mod.QUEUE_STATUS_DISPLAY_EXPORTS,
            *self.mod.QUEUE_RESULT_DISPLAY_EXPORTS,
            *self.mod.QUEUE_LOG_DISPLAY_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_DISPLAY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_queue_display_helpers_routes_focused_groups_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_command_display_helpers") as command,
            mock.patch.object(self.mod, "install_queue_status_display_helpers") as status,
            mock.patch.object(self.mod, "install_queue_result_display_helpers") as result,
            mock.patch.object(self.mod, "install_queue_log_display_helpers") as log,
            mock.patch.object(self.mod, "install_local_helpers") as install,
        ):
            self.mod.install_queue_display_helpers(bindings, names=("summarize_job", "result_overall_line", "external"))

        command.assert_called_once_with(bindings, ("summarize_job",))
        status.assert_called_once_with(bindings, ())
        result.assert_called_once_with(bindings, ("result_overall_line",))
        log.assert_called_once_with(bindings, ())
        install.assert_called_once_with(bindings, self.mod.__dict__, ("external",))


if __name__ == "__main__":
    unittest.main()
