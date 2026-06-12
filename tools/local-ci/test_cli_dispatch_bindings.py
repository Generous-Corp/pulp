#!/usr/bin/env python3
"""Tests for CLI dispatch facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("cli_dispatch_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CliDispatchBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        captured = {}

        def cmd_desktop_config(args, *, commands):
            captured["desktop_config_args"] = args
            captured["desktop_config_commands"] = commands
            return 11

        def dispatch_desktop_command(args, *, commands):
            captured["desktop_args"] = args
            captured["desktop_commands"] = commands
            return 22

        def dispatch_main_command(args, **kwargs):
            captured["main_args"] = args
            captured["main_kwargs"] = kwargs
            return 33

        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(cmd_desktop_config=cmd_desktop_config),
            "_cli_dispatch": types.SimpleNamespace(
                dispatch_desktop_command=dispatch_desktop_command,
                dispatch_main_command=dispatch_main_command,
            ),
        }
        for name in [
            "cmd_desktop_config_show",
            "cmd_desktop_config_set",
            "cmd_desktop_install",
            "cmd_desktop_doctor",
            "cmd_desktop_status",
            "cmd_desktop_config",
            "cmd_desktop_recent",
            "cmd_desktop_proof",
            "cmd_desktop_publish",
            "cmd_desktop_cleanup",
            "cmd_desktop_smoke",
            "cmd_desktop_click",
            "cmd_desktop_inspect",
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

    def test_cmd_desktop_config_binds_facade_commands(self):
        bindings, captured = self._bindings()
        args = object()

        result = self.mod.cmd_desktop_config(bindings, args)

        self.assertEqual(result, 11)
        self.assertIs(captured["desktop_config_args"], args)
        self.assertEqual(set(captured["desktop_config_commands"]), {"show", "set"})
        self.assertIs(captured["desktop_config_commands"]["show"], bindings["cmd_desktop_config_show"])
        self.assertIs(captured["desktop_config_commands"]["set"], bindings["cmd_desktop_config_set"])

    def test_cmd_desktop_binds_facade_commands(self):
        bindings, captured = self._bindings()
        args = object()

        result = self.mod.cmd_desktop(bindings, args)

        self.assertEqual(result, 22)
        self.assertIs(captured["desktop_args"], args)
        self.assertEqual(
            set(captured["desktop_commands"]),
            {"install", "doctor", "status", "config", "recent", "proof", "publish", "cleanup", "smoke", "click", "inspect"},
        )
        self.assertIs(captured["desktop_commands"]["install"], bindings["cmd_desktop_install"])
        self.assertIs(captured["desktop_commands"]["inspect"], bindings["cmd_desktop_inspect"])

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


if __name__ == "__main__":
    unittest.main()
