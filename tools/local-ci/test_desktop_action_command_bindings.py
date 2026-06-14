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

    def test_command_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_ACTION_SELECTOR_EXPORTS,
            *self.mod.DESKTOP_ACTION_RUN_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_ACTION_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

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

    def test_install_desktop_action_command_helpers_routes_each_group(self) -> None:
        def smoke_runner(*args, **kwargs):
            return 5

        bindings = self.bindings("cmd_desktop_smoke", smoke_runner)
        bindings["_desktop_action_commands_cli"].windows_requires_pulp_app_selectors = lambda args: True

        self.mod.install_desktop_action_command_helpers(
            bindings,
            ("windows_requires_pulp_app_selectors", "cmd_desktop_smoke"),
        )

        self.assertTrue(bindings["windows_requires_pulp_app_selectors"](object()))
        self.assertEqual(bindings["cmd_desktop_smoke"](object()), 5)
        self.assertNotIn("cmd_desktop_click", bindings)

    def test_install_desktop_action_command_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_desktop_action_command_helper = lambda _bindings: "future"

        self.mod.install_desktop_action_command_helpers(bindings, ("future_desktop_action_command_helper",))

        self.assertEqual(bindings["future_desktop_action_command_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
