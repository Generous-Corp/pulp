#!/usr/bin/env python3
"""Tests for desktop action command facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_action_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopActionCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def bindings(self, runner_name: str, runner):
        desktop_cli = types.SimpleNamespace(desktop_action_success_lines=object())
        bindings = {
            "_desktop_action_commands_cli": types.SimpleNamespace(**{runner_name: runner}),
            "_desktop_cli": desktop_cli,
            "sys": types.SimpleNamespace(platform="darwin"),
        }
        for name in [
            "load_config",
            "resolve_desktop_target",
            "make_desktop_source_request",
            "run_macos_local_smoke",
            "run_linux_xvfb_remote_action",
            "run_windows_session_agent_action",
        ]:
            bindings[name] = object()
        return bindings

    def test_smoke_binds_action_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 5

        bindings = self.bindings("cmd_desktop_smoke", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_desktop_smoke(bindings, args_obj), 5)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["load_config_fn"], bindings["load_config"])
        self.assertIs(captured["kwargs"]["run_windows_session_agent_action_fn"], bindings["run_windows_session_agent_action"])
        self.assertIs(captured["kwargs"]["desktop_action_success_lines_fn"], bindings["_desktop_cli"].desktop_action_success_lines)
        self.assertEqual(captured["kwargs"]["sys_platform"], "darwin")


if __name__ == "__main__":
    unittest.main()
