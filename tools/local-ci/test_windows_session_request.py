#!/usr/bin/env python3
"""No-network tests for Windows session-agent request helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_session_request.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsSessionRequestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_contract_paths_and_session_agent_request_outputs_and_env(self) -> None:
        contract = self.mod.desktop_target_contract(
            " win target! ",
            {"adapter": "windows-session-agent"},
        )

        self.assertEqual(contract["task_name"], "PulpDesktopAutomationAgent-win-target")
        self.assertEqual(contract["jobs_dir"], r"%LOCALAPPDATA%\Pulp\desktop-automation-agent\jobs")
        self.assertEqual(self.mod.desktop_target_contract("mac", {"adapter": "macos-local"}), {})

        request = self.mod.build_windows_session_agent_request(
            "windows",
            contract,
            r".\build\ui-preview.exe --smoke",
            repo_path=r"C:\Pulp",
            action_name="click",
            label=None,
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
            default_desktop_label_fn=lambda command: "ui-preview",
            uuid_hex_fn=lambda: "job123",
        )

        self.assertEqual(request["job_id"], "job123")
        self.assertEqual(request["label"], "ui-preview")
        self.assertEqual(request["cwd"], r"C:\Pulp")
        self.assertEqual(request["outputs"]["result_root"], contract["results_dir"] + r"\job123")
        self.assertEqual(request["outputs"]["ui_snapshot"], contract["results_dir"] + r"\job123\ui-tree.json")
        self.assertEqual(request["outputs"]["before_screenshot"], contract["results_dir"] + r"\job123\screenshots\before.png")
        self.assertEqual(request["execution"]["capture_mode"], "pulp-app")
        self.assertEqual(request["env"]["PULP_AUTOMATION_CLICK_VIEW_ID"], "bypass-toggle")
        self.assertEqual(request["env"]["PULP_AUTOMATION_AFTER_DELAY_MS"], "750")
        self.assertEqual(request["env"]["PULP_VIEW_TREE_OUT"], request["outputs"]["ui_snapshot"])


if __name__ == "__main__":
    unittest.main()
