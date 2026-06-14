#!/usr/bin/env python3
"""Tests for cloud command facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("cloud_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_command_exports_match_wrappers(self):
        expected = (
            *self.mod.CLOUD_REPORTING_COMMAND_EXPORTS,
            *self.mod.CLOUD_RUN_COMMAND_EXPORTS,
            *self.mod.CLOUD_NAMESPACE_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.CLOUD_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_cloud_command_helpers_routes_each_group_and_unknown_exports(self):
        bindings = {}
        names = (
            "cmd_cloud_history",
            "cmd_cloud_status",
            "cmd_cloud_namespace_setup",
            "custom",
        )

        with (
            mock.patch.object(self.mod, "install_cloud_reporting_command_helpers") as reporting,
            mock.patch.object(self.mod, "install_cloud_run_command_helpers") as run,
            mock.patch.object(self.mod, "install_cloud_namespace_command_helpers") as namespace,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_cloud_command_helpers(bindings, names)

        reporting.assert_called_once_with(bindings, ("cmd_cloud_history",))
        run.assert_called_once_with(bindings, ("cmd_cloud_status",))
        namespace.assert_called_once_with(bindings, ("cmd_cloud_namespace_setup",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
