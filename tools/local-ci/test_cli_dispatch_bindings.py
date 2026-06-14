#!/usr/bin/env python3
"""Tests for CLI dispatch compatibility facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("cli_dispatch_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CliDispatchBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_facade_reexports_desktop_and_main_dispatch_helpers(self):
        expected = (
            *self.mod.CLI_DESKTOP_DISPATCH_EXPORTS,
            *self.mod.CLI_MAIN_DISPATCH_EXPORTS,
        )

        self.assertEqual(self.mod.CLI_DISPATCH_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in self.mod.CLI_DISPATCH_EXPORTS:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_cli_dispatch_helpers_routes_each_group(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_cli_desktop_dispatch_helpers") as install_desktop,
            mock.patch.object(self.mod, "install_cli_main_dispatch_helpers") as install_main,
        ):
            self.mod.install_cli_dispatch_helpers(bindings, ("cmd_desktop", "dispatch_main_command"))

        install_desktop.assert_called_once_with(bindings, ("cmd_desktop",))
        install_main.assert_called_once_with(bindings, ("dispatch_main_command",))

    def test_install_cli_dispatch_helpers_preserves_unknown_fallbacks(self):
        bindings = {"custom": object()}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_cli_dispatch_helpers(bindings, ("custom",))

        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
