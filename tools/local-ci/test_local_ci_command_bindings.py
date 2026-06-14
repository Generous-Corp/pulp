#!/usr/bin/env python3
"""Tests for top-level local-CI command facade composition."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_local_ci_command_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.LOCAL_CI_SUBMISSION_EXPORTS,
            *self.mod.LOCAL_CI_QUEUE_COMMAND_EXPORTS,
            *self.mod.LOCAL_CI_PR_COMMAND_EXPORTS,
            *self.mod.LOCAL_CI_STATUS_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.LOCAL_CI_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_local_ci_command_helpers_routes_pr_local_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_local_ci_pr_command_helpers") as pr,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_local_ci_command_helpers(
                bindings,
                (
                    "resolve_submission_options",
                    "cmd_enqueue",
                    "cmd_ship",
                    "cmd_status",
                    "custom_command_export",
                ),
            )

        pr.assert_called_once_with(bindings, ("cmd_ship",))
        install_local.assert_has_calls(
            [
                mock.call(bindings, self.mod.__dict__, ("resolve_submission_options", "cmd_enqueue", "cmd_status")),
                mock.call(bindings, self.mod.__dict__, ("custom_command_export",)),
            ]
        )


if __name__ == "__main__":
    unittest.main()
