#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("desktop_inspect_action_dispatch.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopInspectActionDispatchTests(unittest.TestCase):
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

    def test_inspect_selects_adapter_snapshot_policy(self):
        runner, error = self.mod.desktop_inspect_runner(
            args=self.args(bundle_id="com.example.App", launch_command=None),
            target={"adapter": "macos-local"},
            sys_platform="darwin",
            **self.deps(),
        )

        self.assertIsNone(error)
        self.assertEqual(runner(), {"label": "macos"})
        self.assertFalse(self.calls[-1][2]["capture_ui_snapshot"])
        self.assertFalse(self.calls[-1][2]["capture_before"])
        self.assertEqual(self.calls[-1][2]["settle_secs"], 0.0)

        runner, error = self.mod.desktop_inspect_runner(
            args=self.args(target="ubuntu", pulp_app_automation=True),
            target={"adapter": "linux-xvfb"},
            sys_platform="darwin",
            **self.deps(),
        )

        self.assertIsNone(error)
        self.assertEqual(runner(), {"label": "linux"})
        self.assertTrue(self.calls[-1][2]["capture_ui_snapshot"])
        self.assertTrue(self.calls[-1][2]["pulp_app_automation"])

    def test_inspect_reports_launch_mode_and_adapter_errors(self):
        runner, error = self.mod.desktop_inspect_runner(
            args=self.args(launch_command=None, bundle_id=None),
            target={"adapter": "macos-local"},
            sys_platform="darwin",
            **self.deps(),
        )
        self.assertIsNone(runner)
        self.assertEqual(error, "desktop inspect requires exactly one of --command or --bundle-id.")

        runner, error = self.mod.desktop_inspect_runner(
            args=self.args(target="other"),
            target={"adapter": "remote-session-agent"},
            sys_platform="darwin",
            **self.deps(),
        )
        self.assertIsNone(runner)
        self.assertIn("desktop inspect is not implemented", error)


if __name__ == "__main__":
    unittest.main()
