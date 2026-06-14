#!/usr/bin/env python3
"""Tests for core state path facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("state_path_core_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class StatePathCoreBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_core_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.STATE_PATH_CONFIG_EXPORTS,
            *self.mod.STATE_PATH_STORE_EXPORTS,
        )

        self.assertEqual(self.mod.STATE_PATH_CORE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_state_path_core_helpers_routes_focused_groups(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_state_path_config_helpers") as config,
            mock.patch.object(self.mod, "install_state_path_store_helpers") as store,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_state_path_core_helpers(
                bindings,
                ("config_path", "queue_path", "custom_state_path_export"),
            )

        config.assert_called_once_with(bindings, ("config_path",))
        store.assert_called_once_with(bindings, ("queue_path",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_state_path_export",))


if __name__ == "__main__":
    unittest.main()
