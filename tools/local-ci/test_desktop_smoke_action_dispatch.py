#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("desktop_smoke_action_dispatch.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSmokeActionDispatchTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.calls: list[tuple[str, tuple, dict]] = []

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

    def test_smoke_selects_macos_and_preserves_options(self):
        runner, error = self.mod.desktop_smoke_runner(
            args=self.args(capture_ui_snapshot=True, click_view_text="Bypass", capture_before=True),
            target={"adapter": "macos-local"},
            sys_platform="darwin",
            **self.deps(),
        )

        self.assertIsNone(error)
        self.assertEqual(runner(), {"label": "macos"})
        self.assertEqual(self.calls[0][0], "macos")
        self.assertEqual(self.calls[0][2]["action_name"], "smoke")
        self.assertTrue(self.calls[0][2]["capture_ui_snapshot"])
        self.assertEqual(self.calls[0][2]["click_view_text"], "Bypass")
        self.assertTrue(self.calls[0][2]["capture_before"])

    def test_smoke_reports_adapter_validation_errors(self):
        runner, error = self.mod.desktop_smoke_runner(
            args=self.args(target="ubuntu", launch_command=None),
            target={"adapter": "linux-xvfb"},
            sys_platform="darwin",
            **self.deps(),
        )
        self.assertIsNone(runner)
        self.assertEqual(error, "desktop smoke requires --command for linux-xvfb targets.")

        runner, error = self.mod.desktop_smoke_runner(
            args=self.args(target="windows", click_view_id="root"),
            target={"adapter": "windows-session-agent"},
            sys_platform="darwin",
            **self.deps(),
        )
        self.assertIsNone(runner)
        self.assertIn("view-target selectors only with --pulp-app-automation", error)


if __name__ == "__main__":
    unittest.main()
