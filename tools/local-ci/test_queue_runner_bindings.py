#!/usr/bin/env python3
"""Tests for queue runner-state facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_runner_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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

    def test_install_queue_runner_helpers_routes_known_and_unknown_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_runner_helpers(
                bindings,
                ("read_runner_info", "stale_running_jobs_unlocked", "update_runner_active_targets", "unknown_helper"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(
                    bindings,
                    self.mod.__dict__,
                    ("read_runner_info", "stale_running_jobs_unlocked", "update_runner_active_targets"),
                ),
                mock.call(bindings, self.mod.__dict__, ("unknown_helper",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
