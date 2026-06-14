#!/usr/bin/env python3
"""Tests for desktop run manifest and rollup dependency bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("desktop_run_rollup_bindings.py")


class DesktopRunRollupBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_run_rollup_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_RUN_MANIFEST_EXPORTS,
            *self.mod.DESKTOP_RUN_ROLLUP_ACTION_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_RUN_ROLLUP_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_run_rollup_helpers_routes_each_group_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_run_manifest_helpers") as install_manifest,
            mock.patch.object(self.mod, "install_desktop_run_rollup_action_helpers") as install_action,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_run_rollup_helpers(
                bindings,
                ("desktop_run_manifests", "write_desktop_run_rollups", "custom"),
            )

        install_manifest.assert_called_once_with(bindings, ("desktop_run_manifests",))
        install_action.assert_called_once_with(bindings, ("write_desktop_run_rollups",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
