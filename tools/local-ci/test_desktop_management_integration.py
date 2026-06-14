#!/usr/bin/env python3
"""Command-level desktop management integration tests."""

from __future__ import annotations

import io
import json
import os
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("local_ci.py", module_name="pulp_local_ci_desktop_management_integration", add_module_dir=True)


class DesktopManagementIntegrationTests(unittest.TestCase):
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

    def _set_target_enabled(self, name: str, enabled: bool):
        payload = json.loads(self.config_path.read_text())
        payload.setdefault("targets", {}).setdefault(name, {})["enabled"] = enabled
        self.config_path.write_text(json.dumps(payload) + "\n")

    def test_cmd_desktop_install_records_receipt(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_install(SimpleNamespace(target="ubuntu"))

        self.assertEqual(exit_code, 0)
        receipt = self.mod.desktop_receipt_for("ubuntu")
        self.assertIsNotNone(receipt)
        self.assertEqual(receipt["target"], "ubuntu")
        self.assertEqual(receipt["adapter"], "linux-xvfb")
        self.assertFalse(receipt["remote_bootstrap_ready"])
        self.assertTrue(Path(receipt["artifact_root"]).exists())
        self.assertIn("Desktop target `ubuntu` prepared.", buf.getvalue())

    def test_cmd_desktop_status_prints_target_summary(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="mac"))

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_status(SimpleNamespace(target="mac"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop automation:", output)
        self.assertIn("mac:", output)
        self.assertIn("adapter: macos-local", output)
        self.assertIn("installed: yes", output)
        self.assertIn("pulp_app_automation", output)

    def test_cmd_desktop_status_prints_windows_contract_summary(self):
        self._set_target_enabled("windows", True)
        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "bootstrap_windows_session_agent",
                return_value={
                    "task_name": "PulpDesktopAutomationAgent-windows",
                    "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                    "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                    "script_exists": True,
                    "jobs_dir_exists": True,
                    "results_dir_exists": True,
                },
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_windows_session_agent",
                    return_value={
                        "task_present": True,
                        "task_state": "Ready",
                        "interactive_user": r"DESKTOP\daniel",
                        "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                        "agent_root_exists": True,
                        "jobs_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\jobs",
                        "jobs_dir_exists": True,
                        "results_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\results",
                        "results_dir_exists": True,
                        "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                        "script_exists": True,
                    },
                ):
                    with mock.patch.object(
                        self.mod,
                        "ensure_windows_remote_tooling",
                        return_value={
                            "probe": {
                                "git_found": True,
                                "git_path": r"C:\Program Files\Git\cmd\git.exe",
                                "git_version": "git version 2.49.0.windows.1",
                                "gh_found": True,
                                "gh_path": r"C:\Program Files\GitHub CLI\gh.exe",
                                "gh_version": "gh version 2.70.0",
                                "gh_auth_ready": True,
                                "gh_auth_detail": "authenticated",
                                "winget_found": True,
                                "winget_path": r"C:\Users\danielraffel\AppData\Local\Microsoft\WindowsApps\winget.exe",
                                "winget_version": "v1.28.220",
                            },
                            "installed": [],
                        },
                    ):
                        with mock.patch.object(
                            self.mod,
                            "ensure_windows_remote_repo_checkout",
                            return_value={
                                "home_dir": r"C:\Users\danielraffel",
                                "repo_path": r"C:\Users\danielraffel\pulp-validate",
                                "repo_exists": True,
                                "git_dir_exists": True,
                                "head_exists": True,
                                "setup_exists": True,
                                "origin_url": "https://github.com/danielraffel/pulp",
                                "repo_path_unsafe": False,
                            },
                        ):
                            with mock.patch.object(
                                self.mod.subprocess,
                                "run",
                                return_value=subprocess.CompletedProcess(["git"], 0, "a" * 40 + "\n", ""),
                            ):
                                with mock.patch.object(
                                    self.mod,
                                    "sync_job_bundle_to_ssh_host",
                                    return_value=("bundle.git", "refs/pulp-ci-bundles/install"),
                                ):
                                    self.mod.cmd_desktop_install(SimpleNamespace(target="windows"))

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_status(SimpleNamespace(target="windows"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("task_name: PulpDesktopAutomationAgent-windows", output)
        self.assertIn(r"remote_root: C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent", output)
        self.assertIn("remote_bootstrap_ready: True", output)
        self.assertIn("remote_tooling_ready: True", output)
        self.assertIn("remote_repo_checkout_ready: True", output)
        self.assertIn(r"remote_git: git version 2.49.0.windows.1 (C:\Program Files\Git\cmd\git.exe)", output)
        self.assertIn(r"remote_gh: gh version 2.70.0 (C:\Program Files\GitHub CLI\gh.exe)", output)
        self.assertIn(r"remote_repo_checkout: C:\Users\danielraffel\pulp-validate (https://github.com/danielraffel/pulp)", output)

    def test_publish_report_to_branch_pushes_report_to_remote_branch(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)
        config["desktop_automation"]["publish_mode"] = "branch"
        config["desktop_automation"]["publish_branch"] = "dev-artifacts-test"

        report_dir = artifact_root / "_published" / "20260404-branch-test"
        (report_dir / "assets" / "run-01").mkdir(parents=True, exist_ok=True)
        (report_dir / "assets" / "run-01" / "window.png").write_bytes(b"png")
        (report_dir / "index.json").write_text(json.dumps({"label": "branch-gallery"}) + "\n")
        (report_dir / "index.html").write_text("<html></html>\n")
        report = {
            "generated_at": "2026-04-04T21:30:00+00:00",
            "label": "branch-gallery",
            "publish_mode": "branch",
            "publish_branch": "dev-artifacts-test",
            "output_dir": str(report_dir),
            "index_html": str(report_dir / "index.html"),
            "index_json": str(report_dir / "index.json"),
            "run_count": 1,
            "runs": [
                {
                    "label": "ui-preview",
                    "target": "mac",
                    "action": "click",
                    "artifacts": {"screenshot": "assets/run-01/window.png"},
                }
            ],
        }

        git_root = Path(self.tmpdir.name) / "git-root"
        remote_root = Path(self.tmpdir.name) / "remote.git"
        git_root.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "init", "--bare", str(remote_root)], check=True, capture_output=True, text=True)
        subprocess.run(["git", "init"], cwd=git_root, check=True, capture_output=True, text=True)
        subprocess.run(["git", "config", "user.name", "Pulp Tests"], cwd=git_root, check=True, capture_output=True, text=True)
        subprocess.run(["git", "config", "user.email", "tests@example.com"], cwd=git_root, check=True, capture_output=True, text=True)
        (git_root / "README.md").write_text("root\n")
        subprocess.run(["git", "add", "README.md"], cwd=git_root, check=True, capture_output=True, text=True)
        subprocess.run(["git", "commit", "-m", "Initial"], cwd=git_root, check=True, capture_output=True, text=True)
        subprocess.run(["git", "remote", "add", "origin", str(remote_root)], cwd=git_root, check=True, capture_output=True, text=True)

        with mock.patch.object(self.mod, "ROOT", git_root):
            published = self.mod.publish_report_to_branch(config, report)

        self.assertEqual(published["mode"], "branch")
        self.assertEqual(published["branch"], "dev-artifacts-test")
        clone_root = Path(self.tmpdir.name) / "clone"
        subprocess.run(["git", "clone", "--branch", "dev-artifacts-test", str(remote_root), str(clone_root)], check=True, capture_output=True, text=True)
        self.assertTrue((clone_root / "desktop-automation" / "reports" / "20260404-branch-test" / "index.json").exists())
        self.assertTrue((clone_root / "desktop-automation" / "latest" / "index.html").exists())

    def test_stage_desktop_publish_report_includes_branch_publish_metadata_when_enabled(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)
        config["desktop_automation"]["publish_mode"] = "branch"
        bundle = self.mod.create_desktop_run_bundle(config, "mac", "inspect")
        screenshot = bundle / "screenshots" / "window.png"
        screenshot.parent.mkdir(parents=True, exist_ok=True)
        screenshot.write_bytes(b"after")
        manifest = {
            "target": "mac",
            "action": "inspect",
            "label": "ui-preview",
            "completed_at": "2026-04-04T06:30:00+00:00",
            "artifacts": {"bundle_dir": str(bundle), "screenshot": str(screenshot)},
        }
        (bundle / "manifest.json").write_text(json.dumps(manifest) + "\n")

        with mock.patch.object(self.mod, "publish_report_to_branch", return_value={"mode": "branch", "branch": "dev-artifacts"}):
            report = self.mod.stage_desktop_publish_report(config, [manifest], label="desktop-gallery")

        self.assertEqual(report["published"]["branch"], "dev-artifacts")

    def test_cmd_desktop_cleanup_removes_old_bundles(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)

        keep_bundle = self.mod.create_desktop_run_bundle(config, "mac", "smoke")
        remove_bundle = self.mod.create_desktop_run_bundle(config, "mac", "smoke")
        keep_manifest = {
            "target": "mac",
            "action": "smoke",
            "label": "keep",
            "completed_at": "2026-04-04T06:31:00+00:00",
            "artifacts": {"bundle_dir": str(keep_bundle)},
        }
        remove_manifest = {
            "target": "mac",
            "action": "smoke",
            "label": "remove",
            "completed_at": "2026-04-01T06:31:00+00:00",
            "artifacts": {"bundle_dir": str(remove_bundle)},
        }
        (keep_bundle / "manifest.json").write_text(json.dumps(keep_manifest) + "\n")
        (remove_bundle / "manifest.json").write_text(json.dumps(remove_manifest) + "\n")

        original_time = self.mod.time.time
        self.mod.time.time = lambda: self.mod.datetime.fromisoformat("2026-04-04T06:31:00+00:00").timestamp()
        try:
            with mock.patch.object(self.mod, "load_config", return_value=config):
                buf = io.StringIO()
                with redirect_stdout(buf):
                    exit_code = self.mod.cmd_desktop_cleanup(
                        SimpleNamespace(target="mac", older_than_days=1, keep_last=1)
                    )
        finally:
            self.mod.time.time = original_time

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop cleanup removed 1 bundle(s).", output)
        self.assertTrue(keep_bundle.exists())
        self.assertFalse(remove_bundle.exists())
        latest_run = json.loads((artifact_root / "mac" / "latest-run.json").read_text())
        runs_jsonl = (artifact_root / "mac" / "runs.jsonl").read_text().strip().splitlines()
        self.assertEqual(latest_run["label"], "keep")
        self.assertEqual(len(runs_jsonl), 1)


if __name__ == "__main__":
    unittest.main()
