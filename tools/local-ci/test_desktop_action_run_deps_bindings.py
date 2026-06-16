#!/usr/bin/env python3
"""Tests for shared desktop action runner dependency bindings."""

from __future__ import annotations

import types
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_action_run_deps_bindings.py")


class DesktopActionRunDepsBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_desktop_action_command_kwargs_bind_shared_dependencies(self) -> None:
        bindings = {
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

        kwargs = self.mod.desktop_action_command_kwargs(bindings)

        for name in [
            "load_config",
            "resolve_desktop_target",
            "make_desktop_source_request",
            "run_macos_local_smoke",
            "run_linux_xvfb_remote_action",
            "run_windows_session_agent_action",
        ]:
            self.assertIs(kwargs[f"{name}_fn"], bindings[name])
        self.assertIs(
            kwargs["desktop_action_success_lines_fn"],
            bindings["_desktop_cli"].desktop_action_success_lines,
        )
        self.assertEqual(kwargs["sys_platform"], "darwin")


if __name__ == "__main__":
    unittest.main()
