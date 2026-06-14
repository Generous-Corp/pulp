#!/usr/bin/env python3
"""Tests for queue runner-state facade bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("queue_runner_bindings.py")


class QueueRunnerBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_runner_exports_match_facade_helpers(self):
        expected = (
            *self.mod.QUEUE_RUNNER_INFO_EXPORTS,
            *self.mod.QUEUE_RUNNER_STALE_EXPORTS,
            *self.mod.QUEUE_RUNNER_ACTIVE_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_RUNNER_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_queue_runner_helpers_routes_focused_groups_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_runner_info_helpers") as info,
            mock.patch.object(self.mod, "install_queue_runner_stale_helpers") as stale,
            mock.patch.object(self.mod, "install_queue_runner_active_helpers") as active,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_queue_runner_helpers(
                bindings,
                ("read_runner_info", "stale_running_jobs_unlocked", "update_runner_active_targets", "unknown_helper"),
            )

        info.assert_called_once_with(bindings, ("read_runner_info",))
        stale.assert_called_once_with(bindings, ("stale_running_jobs_unlocked",))
        active.assert_called_once_with(bindings, ("update_runner_active_targets",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
