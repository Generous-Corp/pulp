#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_action_dispatch.py")


class DesktopActionDispatchTests(unittest.TestCase):
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

    def test_smoke_runner_selects_macos_and_preserves_options(self):
        runner, error = self.mod.desktop_smoke_runner(
            args=self.args(capture_ui_snapshot=True, click_view_text="Bypass", capture_before=True),
            target={"adapter": "macos-local"},
            sys_platform="darwin",
            **self.deps(),
        )

        self.assertIsNone(error)
        self.assertEqual(runner(), {"label": "macos"})
        self.assertEqual(self.calls[0][0], "macos")
        self.assertEqual(self.calls[0][1][1], "app")
        self.assertEqual(self.calls[0][2]["action_name"], "smoke")
        self.assertTrue(self.calls[0][2]["capture_ui_snapshot"])
        self.assertEqual(self.calls[0][2]["click_view_text"], "Bypass")
        self.assertTrue(self.calls[0][2]["capture_before"])
        self.assertEqual(self.calls[0][2]["source_request"], {"mode": "working-tree"})

    def test_click_runner_selects_windows_and_requires_pulp_app_for_view_selectors(self):
        runner, error = self.mod.desktop_click_runner(
            args=self.args(target="windows", click_view_id="root", pulp_app_automation=False),
            target={"adapter": "windows-session-agent"},
            sys_platform="darwin",
            **self.deps(),
        )

        self.assertIsNone(runner)
        self.assertIn("view-target selectors only with --pulp-app-automation", error)

        runner, error = self.mod.desktop_click_runner(
            args=self.args(target="windows", click="10,20", pulp_app_automation=False),
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
        self.assertFalse(self.calls[-1][2]["pulp_app_automation"])

    def test_adapter_validation_and_click_target_helpers(self):
        self.assertFalse(self.mod.windows_requires_pulp_app_selectors(self.args(click="10,20")))
        self.assertTrue(self.mod.windows_requires_pulp_app_selectors(self.args(click_view_label="Bypass")))
        self.assertFalse(self.mod.desktop_click_has_target(self.args()))
        self.assertTrue(self.mod.desktop_click_has_target(self.args(click_view_type="Button")))

        runner, error = self.mod.desktop_smoke_runner(
            args=self.args(target="ubuntu", launch_command=None),
            target={"adapter": "linux-xvfb"},
            sys_platform="darwin",
            **self.deps(),
        )
        self.assertIsNone(runner)
        self.assertEqual(error, "desktop smoke requires --command for linux-xvfb targets.")

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

    def test_inspect_runner_selects_adapter_snapshot_policy(self):
        runner, error = self.mod.desktop_inspect_runner(
            args=self.args(bundle_id="com.example.App", launch_command=None),
            target={"adapter": "macos-local"},
            sys_platform="darwin",
            **self.deps(),
        )

        self.assertIsNone(error)
        self.assertEqual(runner(), {"label": "macos"})
        self.assertEqual(self.calls[-1][0], "macos")
        self.assertEqual(self.calls[-1][2]["action_name"], "inspect")
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
        self.assertEqual(self.calls[-1][0], "linux")
        self.assertTrue(self.calls[-1][2]["capture_ui_snapshot"])
        self.assertTrue(self.calls[-1][2]["pulp_app_automation"])

        runner, error = self.mod.desktop_inspect_runner(
            args=self.args(target="windows", pulp_app_automation=False),
            target={"adapter": "windows-session-agent"},
            sys_platform="darwin",
            **self.deps(),
        )

        self.assertIsNone(error)
        self.assertEqual(runner(), {"label": "windows"})
        self.assertEqual(self.calls[-1][0], "windows")
        self.assertFalse(self.calls[-1][2]["capture_ui_snapshot"])
        self.assertFalse(self.calls[-1][2]["pulp_app_automation"])

    def test_inspect_runner_reports_launch_mode_and_adapter_errors(self):
        runner, error = self.mod.desktop_inspect_runner(
            args=self.args(launch_command=None, bundle_id=None),
            target={"adapter": "macos-local"},
            sys_platform="darwin",
            **self.deps(),
        )
        self.assertIsNone(runner)
        self.assertEqual(error, "desktop inspect requires exactly one of --command or --bundle-id.")

        runner, error = self.mod.desktop_inspect_runner(
            args=self.args(target="ubuntu", bundle_id="com.example.App"),
            target={"adapter": "linux-xvfb"},
            sys_platform="darwin",
            **self.deps(),
        )
        self.assertIsNone(runner)
        self.assertEqual(error, "linux-xvfb desktop inspect currently supports --command only.")

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
