#!/usr/bin/env python3
"""Tests for desktop publish dependency bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("desktop_publish_bindings.py")


class DesktopPublishBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_publish_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_PUBLISH_BRANCH_EXPORTS,
            *self.mod.DESKTOP_PUBLISH_STAGE_EXPORTS,
            *self.mod.DESKTOP_PUBLISH_LIST_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_PUBLISH_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_publish_helpers_routes_each_group_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_publish_branch_helpers") as install_branch,
            mock.patch.object(self.mod, "install_desktop_publish_stage_helpers") as install_stage,
            mock.patch.object(self.mod, "install_desktop_publish_list_helpers") as install_list,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_publish_helpers(
                bindings,
                (
                    "publish_report_to_branch",
                    "stage_desktop_publish_report",
                    "desktop_publish_reports",
                    "custom",
                ),
            )

        install_branch.assert_called_once_with(bindings, ("publish_report_to_branch",))
        install_stage.assert_called_once_with(bindings, ("stage_desktop_publish_report",))
        install_list.assert_called_once_with(bindings, ("desktop_publish_reports",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
