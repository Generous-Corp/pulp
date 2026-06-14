#!/usr/bin/env python3
"""Tests for queue state lifecycle facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_state_lifecycle_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStateLifecycleBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_state_lifecycle_exports_match_facade_helpers(self):
        expected = (
            *self.mod.QUEUE_ACTIVE_LOAD_EXPORTS[:1],
            *self.mod.QUEUE_STALE_STATE_EXPORTS,
            *self.mod.QUEUE_ACTIVE_LOAD_EXPORTS[1:],
        )

        self.assertEqual(self.mod.QUEUE_STATE_LIFECYCLE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_state_lifecycle_helpers_routes_focused_groups_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_active_load_helpers") as active_load,
            mock.patch.object(self.mod, "install_queue_stale_state_helpers") as stale_state,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_queue_state_lifecycle_helpers(
                bindings,
                ("load_job", "update_job_target_state", "custom"),
            )

        active_load.assert_called_once_with(bindings, ("load_job",))
        stale_state.assert_called_once_with(bindings, ("update_job_target_state",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
