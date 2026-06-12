#!/usr/bin/env python3
"""Tests for desktop management command facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_management_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopManagementCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def bindings(self, runner_name: str, runner):
        desktop_cli = types.SimpleNamespace(desktop_recent_lines=object())
        bindings = {
            "_desktop_commands_cli": types.SimpleNamespace(**{runner_name: runner}),
            "_desktop_cli": desktop_cli,
        }
        for name in [
            "load_config",
            "desktop_run_manifests",
            "desktop_run_summary",
            "short_sha",
        ]:
            bindings[name] = object()
        return bindings

    def test_recent_binds_management_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 4

        bindings = self.bindings("cmd_desktop_recent", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_recent(bindings, args_obj), 4)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["desktop_run_manifests_fn"], bindings["desktop_run_manifests"])
        self.assertIs(captured["kwargs"]["desktop_recent_lines_fn"], bindings["_desktop_cli"].desktop_recent_lines)


if __name__ == "__main__":
    unittest.main()
