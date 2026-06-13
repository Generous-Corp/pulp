#!/usr/bin/env python3
"""Tests for top-level CLI dispatch dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("cli_main_dispatch_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CliMainDispatchBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_main_dispatch_exports_are_declared(self):
        self.assertEqual(self.mod.CLI_MAIN_DISPATCH_EXPORTS, ("dispatch_main_command",))

    def _bindings(self):
        captured = {}

        def dispatch_main_command(args, **kwargs):
            captured["main_args"] = args
            captured["main_kwargs"] = kwargs
            return 33

        bindings = {"_cli_dispatch": types.SimpleNamespace(dispatch_main_command=dispatch_main_command)}
        for name in [
            "cmd_enqueue",
            "cmd_drain",
            "cmd_run",
            "cmd_ship",
            "cmd_check",
            "cmd_list",
            "cmd_bump",
            "cmd_cancel",
            "cmd_logs",
            "cmd_cleanup",
            "cmd_evidence",
            "cmd_status",
            "cmd_desktop",
            "cmd_cloud_workflows",
            "cmd_cloud_defaults",
            "cmd_cloud_history",
            "cmd_cloud_compare",
            "cmd_cloud_recommend",
            "cmd_cloud_run",
            "cmd_cloud_status",
            "cmd_cloud_namespace_doctor",
            "cmd_cloud_namespace_setup",
        ]:
            bindings[name] = object()
        return bindings, captured

    def test_dispatch_main_command_binds_top_level_cloud_and_namespace_commands(self):
        bindings, captured = self._bindings()
        args = object()
        print_help = object()

        result = self.mod.dispatch_main_command(bindings, args, print_help)

        self.assertEqual(result, 33)
        self.assertIs(captured["main_args"], args)
        self.assertIs(captured["main_kwargs"]["print_help"], print_help)
        self.assertEqual(
            set(captured["main_kwargs"]["commands"]),
            {"enqueue", "drain", "run", "ship", "check", "list", "bump", "cancel", "logs", "cleanup", "evidence", "status", "desktop"},
        )
        self.assertEqual(
            set(captured["main_kwargs"]["cloud_commands"]),
            {"workflows", "defaults", "history", "compare", "recommend", "run", "status"},
        )
        self.assertEqual(set(captured["main_kwargs"]["cloud_namespace_commands"]), {"doctor", "setup"})
        self.assertIs(captured["main_kwargs"]["commands"]["enqueue"], bindings["cmd_enqueue"])
        self.assertIs(captured["main_kwargs"]["cloud_commands"]["run"], bindings["cmd_cloud_run"])
        self.assertIs(captured["main_kwargs"]["cloud_namespace_commands"]["doctor"], bindings["cmd_cloud_namespace_doctor"])

    def test_install_cli_main_dispatch_helpers_wires_named_exports(self):
        bindings, captured = self._bindings()
        self.mod.install_cli_main_dispatch_helpers(bindings, ("dispatch_main_command",))

        args = object()
        print_help = object()
        self.assertEqual(bindings["dispatch_main_command"](args, print_help), 33)
        self.assertIs(captured["main_args"], args)
        self.assertIs(captured["main_kwargs"]["print_help"], print_help)

    def test_install_cli_main_dispatch_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_cli_main_dispatch_helper = lambda _bindings: "future"

        self.mod.install_cli_main_dispatch_helpers(bindings, ("future_cli_main_dispatch_helper",))

        self.assertEqual(bindings["future_cli_main_dispatch_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
