#!/usr/bin/env python3
from __future__ import annotations

import json
from argparse import Namespace
import unittest

from module_test_utils import load_local_ci_module



def load_desktop_action_commands_cli_module():
    return load_local_ci_module("desktop_action_commands_cli.py")


class DesktopActionCommandsCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_desktop_action_commands_cli_module()
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
            "capture_ui_snapshot": False,
            "click": None,
            "click_view_id": None,
            "click_view_type": None,
            "click_view_text": None,
            "click_view_label": None,
            "capture_before": False,
            "settle_secs": 0.5,
            "timeout": 5.0,
            "pulp_app_automation": False,
            "json": False,
        }
        values.update(overrides)
        return Namespace(**values)

    def deps(self, *, manifest: dict | None = None):
        manifest = manifest or {"label": "preview", "artifacts": {"screenshot": "/tmp/window.png"}}

        def load_config():
            return {"desktop_automation": {"targets": {}}}

        def resolve_desktop_target(_config, name):
            return {"adapter": self.adapters[name], "target_type": "local" if name == "mac" else "ssh"}

        def make_desktop_source_request(args):
            return {"target": args.target, "command": args.launch_command}

        def runner(name):
            def _runner(*args, **kwargs):
                self.calls.append((name, args, kwargs))
                return manifest

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

    def test_windows_selector_helper_detects_view_selectors_only(self):
        self.assertFalse(self.mod.windows_requires_pulp_app_selectors(self.args(click="10,20")))
        self.assertTrue(self.mod.windows_requires_pulp_app_selectors(self.args(click_view_id="root")))
        self.assertTrue(self.mod.windows_requires_pulp_app_selectors(self.args(click_view_label="Bypass")))

    def test_smoke_dispatches_macos_text_and_preserves_options(self):
        result = self.mod.cmd_desktop_smoke(
            self.args(capture_ui_snapshot=True, click_view_text="Bypass", capture_before=True, pulp_app_automation=True),
            **self.deps(),
        )

        self.assertEqual(result, 0)
        self.assertEqual(self.printed, ["smoke:mac:preview"])
        self.assertEqual(self.calls[0][0], "macos")
        self.assertEqual(self.calls[0][1][1], "app")
        self.assertEqual(self.calls[0][2]["action_name"], "smoke")
        self.assertTrue(self.calls[0][2]["capture_ui_snapshot"])
        self.assertEqual(self.calls[0][2]["click_view_text"], "Bypass")
        self.assertTrue(self.calls[0][2]["capture_before"])
        self.assertEqual(self.calls[0][2]["source_request"], {"target": "mac", "command": "app"})

    def test_smoke_json_and_error_paths(self):
        result = self.mod.cmd_desktop_smoke(
            self.args(json=True),
            **self.deps(manifest={"label": "json-run", "artifacts": {"screenshot": "/tmp/json.png"}}),
        )
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(self.printed[-1])["label"], "json-run")

        self.printed.clear()
        deps = self.deps()
        deps["run_macos_local_smoke_fn"] = lambda *_args, **_kwargs: (_ for _ in ()).throw(RuntimeError("boom"))
        result = self.mod.cmd_desktop_smoke(self.args(), **deps)
        self.assertEqual(result, 1)
        self.assertEqual(self.printed, ["Error: boom"])

    def test_adapter_validation_errors_match_existing_commands(self):
        result = self.mod.cmd_desktop_smoke(self.args(target="ubuntu", launch_command=None), **self.deps())
        self.assertEqual(result, 1)
        self.assertIn("requires --command for linux-xvfb", self.printed[-1])

        result = self.mod.cmd_desktop_smoke(
            self.args(target="windows", capture_ui_snapshot=True, pulp_app_automation=False),
            **self.deps(),
        )
        self.assertEqual(result, 1)
        self.assertIn("supports --capture-ui-snapshot only with --pulp-app-automation", self.printed[-1])

        deps = self.deps()
        deps["sys_platform"] = "linux"
        result = self.mod.cmd_desktop_smoke(self.args(target="mac"), **deps)
        self.assertEqual(result, 1)
        self.assertIn("must run on macOS", self.printed[-1])

    def test_click_dispatches_after_selector_validation(self):
        result = self.mod.cmd_desktop_click(self.args(click_view_id="root", capture_ui_snapshot=True), **self.deps())

        self.assertEqual(result, 0)
        self.assertEqual(self.printed, ["click:mac:preview"])
        self.assertEqual(self.calls[0][0], "macos")
        self.assertEqual(self.calls[0][2]["action_name"], "click")
        self.assertTrue(self.calls[0][2]["capture_before"])
        self.assertEqual(self.calls[0][2]["click_view_id"], "root")

        self.printed.clear()
        self.calls.clear()
        result = self.mod.cmd_desktop_click(self.args(click=None), **self.deps())
        self.assertEqual(result, 1)
        self.assertEqual(self.calls, [])
        self.assertIn("requires --click or one view-target selector", self.printed[-1])

    def test_click_windows_rejects_generic_view_selector_but_allows_point(self):
        result = self.mod.cmd_desktop_click(
            self.args(target="windows", click_view_id="root", pulp_app_automation=False),
            **self.deps(),
        )
        self.assertEqual(result, 1)
        self.assertIn("supports view-target selectors only with --pulp-app-automation", self.printed[-1])

        self.printed.clear()
        result = self.mod.cmd_desktop_click(
            self.args(target="windows", click="10,20", pulp_app_automation=False),
            **self.deps(),
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.calls[-1][0], "windows")
        self.assertFalse(self.calls[-1][2]["pulp_app_automation"])
        self.assertEqual(self.calls[-1][2]["click_point"], "10,20")

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
