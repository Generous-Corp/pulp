#!/usr/bin/env python3
"""No-network tests for shared remote desktop action preflight helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_remote_action_preflight.py")


class DesktopRemoteActionPreflightTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_resolve_remote_desktop_action_host_requires_reachable_host_and_repo_path(self) -> None:
        calls = []

        def ensure(target_name, target, defaults):
            calls.append((target_name, target, defaults))
            return target.get("host")

        host, repo_path = self.mod.resolve_remote_desktop_action_host(
            {"defaults": {"ssh_timeout_secs": 2}},
            "ubuntu",
            {"host": "ubuntu.local", "repo_path": "/repo"},
            ensure_host_reachable_fn=ensure,
        )
        self.assertEqual((host, repo_path), ("ubuntu.local", "/repo"))
        self.assertEqual(calls[0][2], {"ssh_timeout_secs": 2})

        with self.assertRaisesRegex(RuntimeError, "not reachable over SSH"):
            self.mod.resolve_remote_desktop_action_host(
                {},
                "ubuntu",
                {"repo_path": "/repo"},
                ensure_host_reachable_fn=lambda *_args: None,
            )

        with self.assertRaisesRegex(RuntimeError, "missing repo_path"):
            self.mod.resolve_remote_desktop_action_host(
                {},
                "ubuntu",
                {"host": "ubuntu.local"},
                ensure_host_reachable_fn=lambda *_args: "ubuntu.local",
            )

    def test_require_pulp_app_automation_for_remote_view_options_preserves_platform_messages(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "snapshot for windows"):
            self.mod.require_pulp_app_automation_for_remote_view_options(
                target_name="windows",
                pulp_app_automation=False,
                capture_ui_snapshot=True,
                click_view_id=None,
                click_view_type=None,
                click_view_text=None,
                click_view_label=None,
                snapshot_error="snapshot for {target_name}",
                selector_error="selector for {target_name}",
            )

        with self.assertRaisesRegex(RuntimeError, "selector for ubuntu"):
            self.mod.require_pulp_app_automation_for_remote_view_options(
                target_name="ubuntu",
                pulp_app_automation=False,
                capture_ui_snapshot=False,
                click_view_id="button",
                click_view_type=None,
                click_view_text=None,
                click_view_label=None,
                snapshot_error="snapshot for {target_name}",
                selector_error="selector for {target_name}",
            )

        self.mod.require_pulp_app_automation_for_remote_view_options(
            target_name="ubuntu",
            pulp_app_automation=True,
            capture_ui_snapshot=True,
            click_view_id="button",
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            snapshot_error="snapshot for {target_name}",
            selector_error="selector for {target_name}",
        )


if __name__ == "__main__":
    unittest.main()
