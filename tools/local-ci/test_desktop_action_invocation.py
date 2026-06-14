#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_action_invocation.py")


class DesktopActionInvocationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.calls: list[tuple[tuple, dict]] = []
        self.config = {"defaults": {}}
        self.source_request = {"mode": "working-tree"}
        self.target = {"adapter": "linux-xvfb"}
        self.args = Namespace(
            target="ubuntu",
            launch_command="/repo/build/ui-preview",
            bundle_id=None,
            label="preview",
            output="/tmp/window.png",
            capture_ui_snapshot=True,
            click="10,20",
            click_view_id="bypass",
            click_view_type="Button",
            click_view_text="Bypass",
            click_view_label="Bypass toggle",
            settle_secs=0.5,
            timeout=5.0,
            pulp_app_automation=True,
        )

    def runner(self, *args, **kwargs):
        self.calls.append((args, kwargs))
        return {"ok": True}

    def test_macos_invocation_preserves_explicit_policy(self) -> None:
        runner = self.mod.macos_desktop_action_runner(
            args=self.args,
            config=self.config,
            source_request=self.source_request,
            action_name="inspect",
            pulp_app_automation=False,
            capture_ui_snapshot=True,
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=False,
            settle_secs=0.0,
            run_macos_local_smoke_fn=self.runner,
        )

        self.assertEqual(runner(), {"ok": True})
        args, kwargs = self.calls[0]
        self.assertEqual(args, (self.config, "/repo/build/ui-preview"))
        self.assertEqual(kwargs["action_name"], "inspect")
        self.assertFalse(kwargs["pulp_app_automation"])
        self.assertTrue(kwargs["capture_ui_snapshot"])
        self.assertIsNone(kwargs["click_point"])
        self.assertIsNone(kwargs["click_view_id"])
        self.assertFalse(kwargs["capture_before"])
        self.assertEqual(kwargs["settle_secs"], 0.0)
        self.assertEqual(kwargs["source_request"], self.source_request)

    def test_linux_invocation_accepts_explicit_selector_policy(self) -> None:
        runner = self.mod.linux_desktop_action_runner(
            args=self.args,
            config=self.config,
            target=self.target,
            source_request=self.source_request,
            action_name="click",
            pulp_app_automation=True,
            capture_ui_snapshot=True,
            click_point="10,20",
            click_view_id="bypass",
            click_view_type="Button",
            click_view_text="Bypass",
            click_view_label="Bypass toggle",
            capture_before=True,
            settle_secs=0.25,
            run_linux_xvfb_remote_action_fn=self.runner,
        )

        self.assertEqual(runner(), {"ok": True})
        args, kwargs = self.calls[0]
        self.assertEqual(args, (self.config, "ubuntu", self.target, "/repo/build/ui-preview"))
        self.assertEqual(kwargs["action_name"], "click")
        self.assertTrue(kwargs["pulp_app_automation"])
        self.assertTrue(kwargs["capture_ui_snapshot"])
        self.assertEqual(kwargs["click_view_label"], "Bypass toggle")
        self.assertTrue(kwargs["capture_before"])
        self.assertEqual(kwargs["settle_secs"], 0.25)

    def test_windows_invocation_uses_validated_automation_value(self) -> None:
        runner = self.mod.windows_desktop_action_runner(
            args=self.args,
            config=self.config,
            target={"adapter": "windows-session-agent"},
            source_request=self.source_request,
            action_name="smoke",
            pulp_app_automation=False,
            capture_ui_snapshot=False,
            click_point="10,20",
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=True,
            settle_secs=0.5,
            run_windows_session_agent_action_fn=self.runner,
        )

        self.assertEqual(runner(), {"ok": True})
        args, kwargs = self.calls[0]
        self.assertEqual(args[1], "ubuntu")
        self.assertEqual(kwargs["action_name"], "smoke")
        self.assertFalse(kwargs["pulp_app_automation"])
        self.assertEqual(kwargs["click_point"], "10,20")
        self.assertIsNone(kwargs["click_view_id"])
        self.assertTrue(kwargs["capture_before"])


if __name__ == "__main__":
    unittest.main()
