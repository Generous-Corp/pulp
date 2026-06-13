#!/usr/bin/env python3
"""Tests for desktop CLI dispatch dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("cli_desktop_dispatch_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CliDesktopDispatchBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_desktop_dispatch_exports_are_declared(self):
        self.assertEqual(self.mod.CLI_DESKTOP_DISPATCH_EXPORTS, ("cmd_desktop_config", "cmd_desktop"))

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

        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(cmd_desktop_config=cmd_desktop_config),
            "_cli_dispatch": types.SimpleNamespace(dispatch_desktop_command=dispatch_desktop_command),
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

    def test_install_cli_desktop_dispatch_helpers_wires_named_exports(self):
        bindings, captured = self._bindings()
        self.mod.install_cli_desktop_dispatch_helpers(bindings, ("cmd_desktop",))

        args = object()
        self.assertEqual(bindings["cmd_desktop"](args), 22)
        self.assertIs(captured["desktop_args"], args)
        self.assertEqual(bindings["cmd_desktop"].__name__, "cmd_desktop")

    def test_install_cli_desktop_dispatch_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_cli_desktop_dispatch_helper = lambda _bindings: "future"

        self.mod.install_cli_desktop_dispatch_helpers(bindings, ("future_cli_desktop_dispatch_helper",))

        self.assertEqual(bindings["future_cli_desktop_dispatch_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
