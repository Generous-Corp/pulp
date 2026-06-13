#!/usr/bin/env python3
"""Tests for CLI dispatch compatibility facade bindings."""

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

    def test_facade_reexports_desktop_and_main_dispatch_helpers(self):
        self.assertEqual(self.mod.CLI_DISPATCH_EXPORTS, ("cmd_desktop_config", "cmd_desktop", "dispatch_main_command"))
        for name in self.mod.CLI_DISPATCH_EXPORTS:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_cli_dispatch_helpers_wires_named_exports(self):
        captured = {}
        args = object()

        def dispatch_desktop_command(value, *, commands):
            captured["desktop_args"] = value
            captured["desktop_commands"] = commands
            return 22

        def dispatch_main_command(value, **kwargs):
            captured["main_args"] = value
            captured["main_kwargs"] = kwargs
            return 33

        bindings = {
            "_cli_dispatch": types.SimpleNamespace(
                dispatch_desktop_command=dispatch_desktop_command,
                dispatch_main_command=dispatch_main_command,
            ),
        }
        for name in [
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

        self.mod.install_cli_dispatch_helpers(bindings, ("cmd_desktop", "dispatch_main_command"))

        self.assertEqual(bindings["cmd_desktop"](args), 22)
        self.assertEqual(bindings["dispatch_main_command"](args, lambda: None), 33)
        self.assertIs(captured["desktop_args"], args)
        self.assertIs(captured["main_args"], args)


if __name__ == "__main__":
    unittest.main()
