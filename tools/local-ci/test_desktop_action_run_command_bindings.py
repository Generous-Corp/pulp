#!/usr/bin/env python3
"""Tests for desktop action runner command facade bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_action_run_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopActionRunCommandBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def bindings(self, runner_name: str, runner):
        bindings = {
            "_desktop_action_commands_cli": types.SimpleNamespace(**{runner_name: runner}),
            "_desktop_cli": types.SimpleNamespace(desktop_action_success_lines=object()),
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

    def test_action_commands_bind_shared_runner_dependencies(self) -> None:
        cases = [
            ("cmd_desktop_smoke", self.mod.cmd_desktop_smoke),
            ("cmd_desktop_click", self.mod.cmd_desktop_click),
            ("cmd_desktop_inspect", self.mod.cmd_desktop_inspect),
        ]
        for runner_name, wrapper in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*args, **kwargs):
                    captured["args"] = args
                    captured["kwargs"] = kwargs
                    return 5

                bindings = self.bindings(runner_name, runner)
                args_obj = object()
                self.assertEqual(wrapper(bindings, args_obj), 5)
                self.assertEqual(captured["args"], (args_obj,))
                for name in [
                    "load_config",
                    "resolve_desktop_target",
                    "make_desktop_source_request",
                    "run_macos_local_smoke",
                    "run_linux_xvfb_remote_action",
                    "run_windows_session_agent_action",
                ]:
                    self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])
                self.assertIs(
                    captured["kwargs"]["desktop_action_success_lines_fn"],
                    bindings["_desktop_cli"].desktop_action_success_lines,
                )
                self.assertEqual(captured["kwargs"]["sys_platform"], "darwin")


if __name__ == "__main__":
    unittest.main()
