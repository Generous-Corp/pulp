#!/usr/bin/env python3
"""No-network tests for local-ci Windows desktop target helpers."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("windows_target.py")


def load_module():
    spec = importlib.util.spec_from_file_location("windows_target_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class WindowsTargetTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_contract_paths_and_repo_safety_helpers(self) -> None:
        contract = self.mod.desktop_target_contract(
            " win target! ",
            {"adapter": "windows-session-agent"},
        )

        self.assertEqual(contract["task_name"], "PulpDesktopAutomationAgent-win-target")
        self.assertEqual(contract["jobs_dir"], r"%LOCALAPPDATA%\Pulp\desktop-automation-agent\jobs")
        self.assertEqual(self.mod.desktop_target_contract("mac", {"adapter": "macos-local"}), {})
        self.assertEqual(self.mod.windows_path_join("", r"C:\Root\\", r"\child", ""), r"C:\Root\child")
        self.assertEqual(self.mod.windows_default_repo_checkout_path(None), "pulp-validate")
        self.assertEqual(
            self.mod.windows_default_repo_checkout_path(r"C:\Users\dev"),
            r"C:\Users\dev\pulp-validate",
        )
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(None))
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(r"C:\\"))
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(r"C:\Users\dev", r"C:\Users\dev"))
        self.assertFalse(self.mod.windows_repo_path_is_unsafe(r"C:\Users\dev\pulp", r"C:\Users\dev"))

        config: dict = {}
        self.mod.update_target_repo_path(config, "windows", r"C:\Pulp")
        self.assertEqual(config["targets"]["windows"]["repo_path"], r"C:\Pulp")
        self.assertEqual(config["desktop_automation"]["targets"]["windows"]["repo_path"], r"C:\Pulp")

    def test_session_agent_request_outputs_and_env(self) -> None:
        contract = self.mod.desktop_target_contract("windows", {"adapter": "windows-session-agent"})
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

    def test_probe_formatting_and_readiness_helpers(self) -> None:
        self.assertFalse(self.mod.windows_repo_checkout_ready(None))
        self.assertFalse(
            self.mod.windows_repo_checkout_ready(
                {"git_dir_exists": True, "head_exists": True, "setup_exists": True, "repo_path_unsafe": True}
            )
        )
        self.assertTrue(
            self.mod.windows_repo_checkout_ready(
                {"git_dir_exists": True, "head_exists": True, "setup_exists": True, "repo_path_unsafe": False}
            )
        )
        self.assertEqual(self.mod.windows_desktop_session_user(None), "")
        self.assertEqual(self.mod.windows_desktop_session_user({"logged_on_user": " dev "}), "dev")
        self.assertEqual(self.mod.windows_desktop_session_state({"session_state": " Active "}), "Active")
        self.assertEqual(
            self.mod.windows_tooling_detail({"git_found": True, "git_path": r"C:\Git\git.exe"}, "git"),
            r"C:\Git\git.exe",
        )
        self.assertEqual(
            self.mod.windows_tooling_detail({"git_found": True, "git_version": "git 2.49", "git_path": "git.exe"}, "git"),
            "git 2.49 (git.exe)",
        )
        self.assertEqual(self.mod.windows_tooling_detail({}, "git", missing_hint="install git"), "install git")
        self.assertTrue(self.mod.windows_remote_tooling_ready({"git_found": True}))
        self.assertFalse(self.mod.windows_remote_tooling_ready({}))
        self.assertIn(
            "not a git checkout",
            self.mod.windows_repo_checkout_detail({"repo_path": r"C:\Pulp", "repo_exists": True}),
        )
        self.assertIn(
            "empty git repo",
            self.mod.windows_repo_checkout_detail({"repo_path": r"C:\Pulp", "git_dir_exists": True}),
        )
        self.assertIn(
            "checkout incomplete",
            self.mod.windows_repo_checkout_detail(
                {"repo_path": r"C:\Pulp", "git_dir_exists": True, "head_exists": True}
            ),
        )
        self.assertEqual(
            self.mod.windows_repo_checkout_detail({"repo_path": r"C:\Pulp", "origin_url": "https://example/repo.git"}),
            r"C:\Pulp (https://example/repo.git)",
        )


if __name__ == "__main__":
    unittest.main()
