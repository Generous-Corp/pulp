#!/usr/bin/env python3
"""Tests for queue status display facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_status_display_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStatusDisplayBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_status_display_exports_match_facade_helpers(self):
        expected = (
            *self.mod.QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS[:2],
            *self.mod.QUEUE_STATUS_TARGET_DISPLAY_EXPORTS,
            self.mod.QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS[2],
            *self.mod.QUEUE_STATUS_RECENT_DISPLAY_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_STATUS_DISPLAY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_queue_status_display_helpers_routes_focused_groups_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_status_active_display_helpers") as active,
            mock.patch.object(self.mod, "install_queue_status_target_display_helpers") as target,
            mock.patch.object(self.mod, "install_queue_status_recent_display_helpers") as recent,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_queue_status_display_helpers(
                bindings,
                ("status_runner_line", "status_target_states", "recent_completed_status_line", "custom_status"),
            )

        active.assert_called_once_with(bindings, ("status_runner_line",))
        target.assert_called_once_with(bindings, ("status_target_states",))
        recent.assert_called_once_with(bindings, ("recent_completed_status_line",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_status",))


if __name__ == "__main__":
    unittest.main()
