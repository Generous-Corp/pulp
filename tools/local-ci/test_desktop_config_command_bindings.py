#!/usr/bin/env python3
"""Tests for desktop config command facade bindings."""

from __future__ import annotations

import types
import unittest
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_config_command_bindings.py")


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
        }

    def test_config_show_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 3

        bindings = self.bindings("cmd_desktop_config_show", runner)
        deps = {"load_config_fn": object(), "desktop_config_show_lines_fn": object()}
        args_obj = object()
        with mock.patch.object(self.mod, "desktop_config_show_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_desktop_config_show(bindings, args_obj), 3)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], deps)

    def test_config_set_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 5

        bindings = self.bindings("cmd_desktop_config_set", runner)
        deps = {
            "load_config_fn": object(),
            "save_config_fn": object(),
            "config_path_fn": object(),
            "normalize_publish_mode_fn": object(),
            "parse_config_bool_fn": object(),
            "normalize_desktop_config_fn": object(),
            "desktop_config_update_lines_fn": object(),
        }
        args_obj = object()
        with mock.patch.object(self.mod, "desktop_config_set_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_desktop_config_set(bindings, args_obj), 5)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], deps)

if __name__ == "__main__":
    unittest.main()
