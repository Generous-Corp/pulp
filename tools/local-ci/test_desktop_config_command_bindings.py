#!/usr/bin/env python3
"""Tests for desktop config command facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_config_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopConfigCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_command_exports_are_declared(self) -> None:
        self.assertEqual(
            self.mod.DESKTOP_CONFIG_COMMAND_EXPORTS,
            ("cmd_desktop_config_show", "cmd_desktop_config_set"),
        )

    def bindings(self, runner_name: str, runner):
        return {
            "_desktop_commands_cli": types.SimpleNamespace(**{runner_name: runner}),
            "_desktop_cli": types.SimpleNamespace(
                desktop_config_show_lines=object(),
                desktop_config_update_lines=object(),
            ),
            "load_config": object(),
            "save_config": object(),
            "config_path": object(),
            "normalize_publish_mode": object(),
            "parse_config_bool": object(),
            "normalize_desktop_config": object(),
        }

    def test_config_show_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 3

        bindings = self.bindings("cmd_desktop_config_show", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_config_show(bindings, args_obj), 3)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(
            captured["kwargs"]["desktop_config_show_lines_fn"],
            bindings["_desktop_cli"].desktop_config_show_lines,
        )

    def test_config_set_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 5

        bindings = self.bindings("cmd_desktop_config_set", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_config_set(bindings, args_obj), 5)
        self.assertEqual(captured["args"], (args_obj,))
        for name in [
            "load_config",
            "save_config",
            "config_path",
            "normalize_publish_mode",
            "parse_config_bool",
            "normalize_desktop_config",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])
        self.assertIs(
            captured["kwargs"]["desktop_config_update_lines_fn"],
            bindings["_desktop_cli"].desktop_config_update_lines,
        )

    def test_install_desktop_config_command_helpers_wires_named_exports(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 13

        bindings = self.bindings("cmd_desktop_config_show", runner)

        self.mod.install_desktop_config_command_helpers(bindings, ("cmd_desktop_config_show",))

        args_obj = object()
        self.assertEqual(bindings["cmd_desktop_config_show"](args_obj), 13)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertNotIn("cmd_desktop_config_set", bindings)

    def test_install_desktop_config_command_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_desktop_config_command_helper = lambda _bindings: "future"

        self.mod.install_desktop_config_command_helpers(bindings, ("future_desktop_config_command_helper",))

        self.assertEqual(bindings["future_desktop_config_command_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
