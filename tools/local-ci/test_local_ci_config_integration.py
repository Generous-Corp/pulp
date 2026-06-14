#!/usr/bin/env python3
"""Command-level local CI configuration integration tests."""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("local_ci.py", module_name="pulp_local_ci_config_integration", add_module_dir=True)


class LocalCiConfigIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        root = Path(self.tmpdir.name)
        self.state_dir = root / "state"
        self.config_path = root / "config.json"
        self.config_path.write_text(
            json.dumps(
                {
                    "desktop_automation": {
                        "artifact_root": str(root / "desktop-artifacts"),
                    },
                    "targets": {
                        "mac": {"type": "local", "enabled": True},
                        "ubuntu": {"type": "ssh", "enabled": True, "host": "ubuntu", "repo_path": "/tmp/pulp"},
                        "windows": {"type": "ssh", "enabled": False, "host": "win2", "repo_path": "C:\\Pulp"},
                    },
                    "github_actions": {
                        "repository": "danielraffel/pulp",
                        "defaults": {
                            "workflow": "build",
                            "provider": "github-hosted",
                            "wait_poll_secs": 5,
                            "match_timeout_secs": 30,
                        },
                    },
                    "defaults": {
                        "priority": "normal",
                        "targets": ["mac"],
                    },
                }
            )
            + "\n"
        )

        self.prev_home = os.environ.get("PULP_LOCAL_CI_HOME")
        self.prev_config = os.environ.get("PULP_LOCAL_CI_CONFIG")
        os.environ["PULP_LOCAL_CI_HOME"] = str(self.state_dir)
        os.environ["PULP_LOCAL_CI_CONFIG"] = str(self.config_path)
        self.mod = load_module()

    def tearDown(self):
        if self.prev_home is None:
            os.environ.pop("PULP_LOCAL_CI_HOME", None)
        else:
            os.environ["PULP_LOCAL_CI_HOME"] = self.prev_home

        if self.prev_config is None:
            os.environ.pop("PULP_LOCAL_CI_CONFIG", None)
        else:
            os.environ["PULP_LOCAL_CI_CONFIG"] = self.prev_config

        self.tmpdir.cleanup()

    def test_desktop_target_overrides_replace_host_and_repo_path(self):
        payload = json.loads(self.config_path.read_text())
        payload.setdefault("desktop_automation", {}).setdefault("targets", {}).setdefault("windows", {}).update(
            {
                "enabled": True,
                "host": "win",
                "repo_path": r"C:\Users\daniel\Code\pulp-validate",
            }
        )
        self.config_path.write_text(json.dumps(payload) + "\n")

        config = self.mod.load_config()
        target = self.mod.resolve_desktop_target(config, "windows")

        self.assertEqual(target["host"], "win")
        self.assertEqual(target["repo_path"], r"C:\Users\daniel\Code\pulp-validate")

    def test_config_path_prefers_shared_state_config(self):
        original_override = os.environ.pop("PULP_LOCAL_CI_CONFIG", None)
        shared_config = self.state_dir / "config.json"
        shared_config.parent.mkdir(parents=True, exist_ok=True)
        shared_config.write_text(
            json.dumps(
                {
                    "targets": {"mac": {"type": "local", "enabled": True}},
                    "defaults": {"priority": "normal", "targets": ["mac"]},
                }
            )
            + "\n"
        )
        try:
            self.assertEqual(self.mod.config_path(), shared_config)
        finally:
            if original_override is None:
                os.environ.pop("PULP_LOCAL_CI_CONFIG", None)
            else:
                os.environ["PULP_LOCAL_CI_CONFIG"] = original_override

    def test_load_config_normalizes_desktop_automation_defaults(self):
        config = self.mod.load_config()

        desktop = config["desktop_automation"]
        self.assertEqual(desktop["publish_mode"], "none")
        self.assertEqual(desktop["publish_branch"], "dev-artifacts")
        self.assertEqual(desktop["retention_days"], 14)
        self.assertTrue(desktop["artifact_root"])
        self.assertEqual(desktop["targets"]["mac"]["adapter"], "macos-local")
        self.assertEqual(desktop["targets"]["mac"]["capability_tier"], "v2")
        self.assertEqual(desktop["targets"]["ubuntu"]["adapter"], "linux-xvfb")
        self.assertEqual(desktop["targets"]["windows"]["adapter"], "windows-session-agent")
        self.assertEqual(desktop["targets"]["windows"]["capability_tier"], "v2")
        self.assertEqual(desktop["targets"]["windows"]["task_name"], None)
        self.assertEqual(desktop["targets"]["windows"]["remote_root"], None)
        contract = self.mod.desktop_target_contract("windows", desktop["targets"]["windows"])
        self.assertEqual(contract["task_name"], "PulpDesktopAutomationAgent-windows")
        self.assertTrue(contract["remote_root"].startswith("%LOCALAPPDATA%"))

    def test_build_windows_session_agent_request_sets_outputs_and_env(self):
        config = self.mod.load_config()
        contract = self.mod.desktop_target_contract("windows", config["desktop_automation"]["targets"]["windows"])
        request = self.mod.build_windows_session_agent_request(
            "windows",
            contract,
            r"C:\Pulp\build\ui-preview.exe",
            repo_path=r"C:\Pulp",
            action_name="click",
            label="ui-preview",
            pulp_app_automation=True,
            capture_ui_snapshot=True,
            click_point=None,
            click_view_id="bypass-toggle",
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=True,
            settle_secs=0.75,
            timeout_secs=20.0,
        )

        self.assertEqual(request["schema"], 1)
        self.assertEqual(request["target"], "windows")
        self.assertEqual(request["action"], "click")
        self.assertEqual(request["cwd"], r"C:\Pulp")
        self.assertEqual(request["execution"]["capture_mode"], "pulp-app")
        self.assertTrue(request["execution"]["capture_ui_snapshot"])
        self.assertTrue(request["execution"]["capture_before"])
        self.assertEqual(request["interaction"]["view_id"], "bypass-toggle")
        self.assertEqual(request["env"]["PULP_AUTOMATION_CLICK_VIEW_ID"], "bypass-toggle")
        self.assertEqual(request["env"]["PULP_AUTOMATION_AFTER_DELAY_MS"], "750")
        self.assertIn("\\results\\", request["outputs"]["screenshot"])
        self.assertIn("ui-tree.json", request["outputs"]["ui_snapshot"])
        self.assertIn("before.png", request["outputs"]["before_screenshot"])


if __name__ == "__main__":
    unittest.main()
