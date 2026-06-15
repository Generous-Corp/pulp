#!/usr/bin/env python3
"""Tests for cloud/GitHub facade composition."""

import unittest
from unittest import mock

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("cloud_bindings.py")


class CloudBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_cloud_helper_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.CLOUD_MODULE_ATTR_EXPORTS,
            *self.mod.CLOUD_COMMAND_EXPORTS,
            *self.mod.CLOUD_GITHUB_EXPORTS,
            *self.mod.CLOUD_RECORD_EXPORTS,
        )

        self.assertEqual(self.mod.CLOUD_HELPER_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_cloud_helpers_routes_each_group(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_cloud_module_attr_helpers") as install_module_attrs,
            mock.patch.object(self.mod, "install_cloud_command_helpers") as install_commands,
            mock.patch.object(self.mod, "install_cloud_github_helpers") as install_github,
            mock.patch.object(self.mod, "install_cloud_record_helpers") as install_records,
        ):
            self.mod.install_cloud_helpers(
                bindings,
                (
                    "summarize_runner_selector",
                    "cmd_cloud_status",
                    "gh_pr_head",
                    "format_ci_comment",
                    "custom_cloud_export",
                ),
            )

        self.assertEqual(
            install_module_attrs.call_args_list,
            [
                mock.call(bindings, ("summarize_runner_selector",)),
                mock.call(bindings, ("custom_cloud_export",)),
            ],
        )
        install_commands.assert_called_once_with(bindings, ("cmd_cloud_status",))
        install_github.assert_called_once_with(bindings, ("gh_pr_head",))
        install_records.assert_called_once_with(bindings, ("format_ci_comment",))


if __name__ == "__main__":
    unittest.main()
