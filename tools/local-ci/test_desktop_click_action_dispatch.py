#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_click_action_dispatch.py")


class DesktopClickActionDispatchTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.calls: list[tuple[str, tuple, dict]] = []

    def args(self, **overrides):
        values = {
            "target": "windows",
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
            "settle_secs": 0.5,
            "timeout": 5.0,
            "pulp_app_automation": False,
        }
        values.update(overrides)
        return Namespace(**values)

    def runner(self, name):
        def _runner(*args, **kwargs):
            self.calls.append((name, args, kwargs))
            return {"label": name}

        return _runner

    def deps(self):
        return {
            "config": {"desktop_automation": {"targets": {}}},
            "source_request": {"mode": "working-tree"},
            "run_macos_local_smoke_fn": self.runner("macos"),
            "run_linux_xvfb_remote_action_fn": self.runner("linux"),
            "run_windows_session_agent_action_fn": self.runner("windows"),
        }

    def test_click_selects_windows_and_requires_pulp_app_for_view_selectors(self):
        runner, error = self.mod.desktop_click_runner(
            args=self.args(click_view_id="root"),
            target={"adapter": "windows-session-agent"},
            sys_platform="darwin",
            **self.deps(),
        )
        self.assertIsNone(runner)
        self.assertIn("view-target selectors only with --pulp-app-automation", error)

        runner, error = self.mod.desktop_click_runner(
            args=self.args(click="10,20"),
            target={"adapter": "windows-session-agent"},
            sys_platform="darwin",
            **self.deps(),
        )
        self.assertIsNone(error)
        self.assertEqual(runner(), {"label": "windows"})
        self.assertEqual(self.calls[-1][0], "windows")
        self.assertEqual(self.calls[-1][2]["action_name"], "click")
        self.assertTrue(self.calls[-1][2]["capture_before"])
        self.assertEqual(self.calls[-1][2]["click_point"], "10,20")

    def test_click_reports_adapter_validation_errors(self):
        runner, error = self.mod.desktop_click_runner(
            args=self.args(target="mac"),
            target={"adapter": "macos-local"},
            sys_platform="linux",
            **self.deps(),
        )
        self.assertIsNone(runner)
        self.assertIn("must run on macOS", error)

        runner, error = self.mod.desktop_click_runner(
            args=self.args(target="other"),
            target={"adapter": "remote-session-agent"},
            sys_platform="darwin",
            **self.deps(),
        )
        self.assertIsNone(runner)
        self.assertIn("desktop click is not implemented", error)


if __name__ == "__main__":
    unittest.main()
