#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
import unittest

from module_test_utils import load_local_ci_module



def load_cli_dispatch_module():
    return load_local_ci_module("cli_dispatch.py")


class CliDispatchTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_cli_dispatch_module()
        self.printed: list[str] = []
        self.help_calls = 0

    def record_help(self):
        self.help_calls += 1

    def record_print(self, line: str):
        self.printed.append(line)

    @staticmethod
    def handler(value: int):
        def run(_args):
            return value

        return run

    def test_dispatch_main_top_level_command(self):
        args = Namespace(command="status")

        result = self.mod.dispatch_main_command(
            args,
            commands={"status": self.handler(7)},
            cloud_commands={},
            cloud_namespace_commands={},
            print_help=self.record_help,
            print_fn=self.record_print,
        )

        self.assertEqual(result, 7)
        self.assertEqual(self.printed, [])
        self.assertEqual(self.help_calls, 0)

    def test_dispatch_main_cloud_command(self):
        args = Namespace(command="cloud", cloud_command="history", cloud_namespace_command=None)

        result = self.mod.dispatch_main_command(
            args,
            commands={},
            cloud_commands={"history": self.handler(8)},
            cloud_namespace_commands={},
            print_help=self.record_help,
            print_fn=self.record_print,
        )

        self.assertEqual(result, 8)
        self.assertEqual(self.printed, [])
        self.assertEqual(self.help_calls, 0)

    def test_dispatch_main_cloud_namespace_command(self):
        args = Namespace(command="cloud", cloud_command="namespace", cloud_namespace_command="doctor")

        result = self.mod.dispatch_main_command(
            args,
            commands={},
            cloud_commands={},
            cloud_namespace_commands={"doctor": self.handler(9)},
            print_help=self.record_help,
            print_fn=self.record_print,
        )

        self.assertEqual(result, 9)
        self.assertEqual(self.printed, [])
        self.assertEqual(self.help_calls, 0)

    def test_dispatch_main_cloud_missing_subcommands(self):
        namespace_args = Namespace(command="cloud", cloud_command="namespace", cloud_namespace_command=None)
        cloud_args = Namespace(command="cloud", cloud_command=None, cloud_namespace_command=None)

        namespace_result = self.mod.dispatch_main_command(
            namespace_args,
            commands={},
            cloud_commands={},
            cloud_namespace_commands={},
            print_help=self.record_help,
            print_fn=self.record_print,
        )
        cloud_result = self.mod.dispatch_main_command(
            cloud_args,
            commands={},
            cloud_commands={},
            cloud_namespace_commands={},
            print_help=self.record_help,
            print_fn=self.record_print,
        )

        self.assertEqual(namespace_result, 1)
        self.assertEqual(cloud_result, 1)
        self.assertIn("missing cloud namespace subcommand", self.printed[0])
        self.assertIn("missing cloud subcommand", self.printed[1])
        self.assertEqual(self.help_calls, 0)

    def test_dispatch_main_unknown_command_prints_help(self):
        args = Namespace(command=None)

        result = self.mod.dispatch_main_command(
            args,
            commands={},
            cloud_commands={},
            cloud_namespace_commands={},
            print_help=self.record_help,
            print_fn=self.record_print,
        )

        self.assertEqual(result, 1)
        self.assertEqual(self.printed, [])
        self.assertEqual(self.help_calls, 1)

    def test_dispatch_desktop_command_and_missing_subcommand(self):
        ok_args = Namespace(desktop_command="status")
        missing_args = Namespace(desktop_command=None)

        ok_result = self.mod.dispatch_desktop_command(
            ok_args,
            commands={"status": self.handler(5)},
            print_fn=self.record_print,
        )
        missing_result = self.mod.dispatch_desktop_command(
            missing_args,
            commands={"status": self.handler(5)},
            print_fn=self.record_print,
        )

        self.assertEqual(ok_result, 5)
        self.assertEqual(missing_result, 1)
        self.assertIn("desktop subcommand required", self.printed[0])


if __name__ == "__main__":
    unittest.main()
