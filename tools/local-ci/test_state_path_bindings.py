#!/usr/bin/env python3
"""Tests for state path facade bindings."""

from module_test_utils import load_module_from_path
from unittest import mock
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("state_path_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class StatePathBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_state_path_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.STATE_PATH_CORE_EXPORTS,
            *self.mod.STATE_PATH_ARTIFACT_EXPORTS,
            *self.mod.STATE_PATH_LOCK_EXPORTS,
            *self.mod.STATE_PATH_LOG_EXPORTS,
        )

        self.assertEqual(self.mod.STATE_PATH_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_state_path_helpers_routes_focused_groups(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_state_path_core_helpers") as install_core,
            mock.patch.object(self.mod, "install_state_path_artifact_helpers") as install_artifact,
            mock.patch.object(self.mod, "install_state_path_lock_helpers") as install_lock,
            mock.patch.object(self.mod, "install_state_path_log_helpers") as install_log,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_state_path_helpers(
                bindings,
                (
                    "state_dir",
                    "bundles_dir",
                    "queue_lock_path",
                    "prepare_target_log",
                    "custom_state_path",
                ),
            )

        install_core.assert_called_once_with(bindings, ("state_dir",))
        install_artifact.assert_called_once_with(bindings, ("bundles_dir",))
        install_lock.assert_called_once_with(bindings, ("queue_lock_path",))
        install_log.assert_called_once_with(bindings, ("prepare_target_log",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_state_path",))


if __name__ == "__main__":
    unittest.main()
