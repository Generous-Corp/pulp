#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
import unittest

from module_test_utils import load_local_ci_module



def load_desktop_inspect_commands_cli_module():
    return load_local_ci_module("desktop_inspect_commands_cli.py")


class DesktopInspectCommandsCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_desktop_inspect_commands_cli_module()
        self.printed: list[str] = []
        self.calls: list[tuple[str, tuple, dict]] = []
        self.adapters = {
            "mac": "macos-local",
            "ubuntu": "linux-xvfb",
            "windows": "windows-session-agent",
            "other": "remote-session-agent",
        }

    def print_line(self, line: str):
        self.printed.append(line)

    def args(self, **overrides):
        values = {
            "target": "mac",
            "launch_command": "app",
            "bundle_id": None,
            "label": "preview",
            "output": "/tmp/window.png",
            "timeout": 5.0,
            "pulp_app_automation": False,
            "json": False,
        }
        values.update(overrides)
        return Namespace(**values)

    def deps(self):
        def load_config():
            return {"desktop_automation": {"targets": {}}}

        def resolve_desktop_target(_config, name):
            return {"adapter": self.adapters[name], "target_type": "local" if name == "mac" else "ssh"}

        def make_desktop_source_request(args):
            return {"target": args.target, "command": args.launch_command}

        def runner(name):
            def _runner(*args, **kwargs):
                self.calls.append((name, args, kwargs))
                return {"label": "preview", "artifacts": {"screenshot": "/tmp/window.png"}}

            return _runner

        return {
            "load_config_fn": load_config,
            "resolve_desktop_target_fn": resolve_desktop_target,
            "make_desktop_source_request_fn": make_desktop_source_request,
            "run_macos_local_smoke_fn": runner("macos"),
            "run_linux_xvfb_remote_action_fn": runner("linux"),
            "run_windows_session_agent_action_fn": runner("windows"),
            "desktop_action_success_lines_fn": lambda action, target, payload: [f"{action}:{target}:{payload['label']}"],
            "sys_platform": "darwin",
            "print_fn": self.print_line,
        }

    def test_inspect_dispatches_adapter_specific_snapshot_policy(self):
        result = self.mod.cmd_desktop_inspect(self.args(bundle_id="com.example.App", launch_command=None), **self.deps())
        self.assertEqual(result, 0)
        self.assertEqual(self.calls[0][0], "macos")
        self.assertEqual(self.calls[0][2]["action_name"], "inspect")
        self.assertFalse(self.calls[0][2]["capture_ui_snapshot"])
        self.assertFalse(self.calls[0][2]["capture_before"])
        self.assertEqual(self.calls[0][2]["settle_secs"], 0.0)

        self.calls.clear()
        result = self.mod.cmd_desktop_inspect(
            self.args(target="ubuntu", pulp_app_automation=True),
            **self.deps(),
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.calls[0][0], "linux")
        self.assertTrue(self.calls[0][2]["capture_ui_snapshot"])

        self.calls.clear()
        result = self.mod.cmd_desktop_inspect(
            self.args(target="windows", pulp_app_automation=False),
            **self.deps(),
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.calls[0][0], "windows")
        self.assertFalse(self.calls[0][2]["capture_ui_snapshot"])

    def test_inspect_reports_launch_mode_and_adapter_errors(self):
        result = self.mod.cmd_desktop_inspect(self.args(launch_command=None, bundle_id=None), **self.deps())
        self.assertEqual(result, 1)
        self.assertIn("requires exactly one of --command or --bundle-id", self.printed[-1])

        result = self.mod.cmd_desktop_inspect(self.args(target="other"), **self.deps())
        self.assertEqual(result, 1)
        self.assertIn("desktop inspect is not implemented", self.printed[-1])


if __name__ == "__main__":
    unittest.main()
