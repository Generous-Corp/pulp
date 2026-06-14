#!/usr/bin/env python3
"""Tests for queue stale/target-state facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_stale_state_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStaleStateBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_stale_state_exports_compose_focused_groups(self):
        expected = (
            *self.mod.QUEUE_STALE_RECONCILE_EXPORTS,
            *self.mod.QUEUE_TARGET_UPDATE_EXPORTS,
            *self.mod.QUEUE_STALE_RECLAIM_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_STALE_STATE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_queue_stale_state_helpers_routes_focused_groups_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_stale_reconcile_helpers") as reconcile,
            mock.patch.object(self.mod, "install_queue_target_update_helpers") as target_update,
            mock.patch.object(self.mod, "install_queue_stale_reclaim_helpers") as reclaim,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_queue_stale_state_helpers(
                bindings,
                (
                    "reconcile_running_jobs_unlocked",
                    "update_job_target_state",
                    "reclaim_stale_remote_validators",
                    "custom_stale_state",
                ),
            )

        reconcile.assert_called_once_with(bindings, ("reconcile_running_jobs_unlocked",))
        target_update.assert_called_once_with(bindings, ("update_job_target_state",))
        reclaim.assert_called_once_with(bindings, ("reclaim_stale_remote_validators",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_stale_state",))


if __name__ == "__main__":
    unittest.main()
