#!/usr/bin/env python3
"""Tests for utility command facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("utility_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class UtilityCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_compose_focused_command_groups(self):
        self.assertEqual(
            self.mod.UTILITY_COMMAND_EXPORTS,
            (
                *self.mod.CLEANUP_COMMAND_EXPORTS,
                *self.mod.UTILITY_QUEUE_COMMAND_EXPORTS,
                *self.mod.LOGS_COMMAND_EXPORTS,
                *self.mod.EVIDENCE_COMMAND_EXPORTS,
            ),
        )
        self.assertEqual(len(self.mod.UTILITY_COMMAND_EXPORTS), len(set(self.mod.UTILITY_COMMAND_EXPORTS)))

    def test_install_utility_command_helpers_routes_known_and_unknown_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install:
            self.mod.install_utility_command_helpers(bindings, names=("cmd_cleanup", "cmd_bump", "external"))

        self.assertEqual(
            install.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("cmd_cleanup", "cmd_bump")),
                mock.call(bindings, self.mod.__dict__, ("external",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
