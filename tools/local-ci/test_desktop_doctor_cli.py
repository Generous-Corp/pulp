#!/usr/bin/env python3
"""Command-level desktop doctor integration tests."""

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
    return load_local_ci_module("local_ci.py", module_name="pulp_local_ci_desktop_doctor_cli", add_module_dir=True)


class DesktopDoctorCliTests(unittest.TestCase):
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

    def _install_windows_target(self, *, session_probe=None, tooling_probe=None):
        self._set_target_enabled("windows", True)
        session_probe = session_probe or {
            "task_name": "PulpDesktopAutomationAgent-windows",
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
        }
        tooling_probe = tooling_probe or {
            "probe": {
                "git_found": True,
                "git_path": r"C:\Program Files\Git\cmd\git.exe",
                "git_version": "git version 2.49.0.windows.1",
                "gh_found": False,
                "winget_found": True,
                "gh_auth_ready": None,
            },
            "installed": [],
        }

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
                with mock.patch.object(self.mod, "probe_windows_session_agent", return_value=session_probe):
                    with mock.patch.object(self.mod, "ensure_windows_remote_tooling", return_value=tooling_probe):
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
                            with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
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

    def _run_windows_doctor(self, *, session_probe=None):
        session_probe = session_probe or {
            "task_name": "PulpDesktopAutomationAgent-windows",
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
        }
        with mock.patch.object(
            self.mod,
            "probe_windows_remote_tooling",
            return_value={
                "git_found": True,
                "git_path": r"C:\Program Files\Git\cmd\git.exe",
                "git_version": "git version 2.49.0.windows.1",
                "gh_found": False,
                "gh_path": "",
                "gh_version": "",
                "gh_auth_ready": None,
                "gh_auth_detail": "",
                "winget_found": True,
                "winget_path": r"C:\Users\danielraffel\AppData\Local\Microsoft\WindowsApps\winget.exe",
                "winget_version": "v1.28.220",
            },
        ):
            with mock.patch.object(
                self.mod,
                "probe_windows_session_agent",
                return_value=session_probe,
            ):
                with mock.patch.object(
                    self.mod,
                "probe_windows_repo_checkout",
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
                    buf = io.StringIO()
                    with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
                        with redirect_stdout(buf):
                            exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="windows"))
        return exit_code, buf.getvalue()

    def test_cmd_desktop_doctor_reports_remote_target_health(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="ubuntu"))

        with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
            with mock.patch.object(
                self.mod,
                "probe_linux_launch_backend",
                return_value={"mode": "xvfb", "path": "/usr/bin/xvfb-run"},
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_linux_remote_tooling",
                    return_value={
                        "git_found": True,
                        "git_version": "git version 2.49.0",
                        "git_path": "/usr/bin/git",
                        "git_lfs_found": True,
                        "git_lfs_version": "git-lfs/3.5.1",
                        "git_lfs_path": "/usr/bin/git-lfs",
                        "xvfb_run_found": True,
                        "xvfb_run_version": "Usage: xvfb-run [OPTION ...] COMMAND",
                        "xvfb_run_path": "/usr/bin/xvfb-run",
                        "xauth_found": True,
                        "xauth_version": "1.1.2",
                        "xauth_path": "/usr/bin/xauth",
                        "xdotool_found": True,
                        "xdotool_version": "xdotool version 3.20211022.1",
                        "xdotool_path": "/usr/bin/xdotool",
                        "xwininfo_found": True,
                        "xwininfo_version": "xwininfo 1.3.4",
                        "xwininfo_path": "/usr/bin/xwininfo",
                        "import_found": True,
                        "import_version": "Version: ImageMagick 6.9.12-98",
                        "import_path": "/usr/bin/import",
                        "wmctrl_found": True,
                        "wmctrl_version": "1.07",
                        "wmctrl_path": "/usr/bin/wmctrl",
                    },
                ):
                    buf = io.StringIO()
                    with redirect_stdout(buf):
                        exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="ubuntu"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop doctor for `ubuntu`", output)
        self.assertIn("PASS  receipt: installed", output)
        self.assertIn("PASS  ssh: ubuntu", output)
        self.assertIn("PASS  launch_backend: /usr/bin/xvfb-run", output)
        self.assertIn("PASS  git-lfs: git-lfs/3.5.1 (/usr/bin/git-lfs)", output)
        self.assertIn("PASS  xdotool: xdotool version 3.20211022.1 (/usr/bin/xdotool)", output)

    def test_cmd_desktop_doctor_reports_linux_xvfb_remediation(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="ubuntu"))

        with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
            with mock.patch.object(
                self.mod,
                "probe_linux_launch_backend",
                return_value={"mode": "missing"},
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_linux_remote_tooling",
                    return_value={
                        "git_found": True,
                        "git_version": "git version 2.49.0",
                        "git_path": "/usr/bin/git",
                        "git_lfs_found": False,
                        "git_lfs_path": "/home/daniel/.local/bin/git-lfs",
                        "git_lfs_version": "git-lfs/3.7.0",
                        "git_lfs_hint": "installed at /home/daniel/.local/bin/git-lfs but unavailable to non-interactive shells; add $HOME/.local/bin to PATH or install git-lfs system-wide",
                        "xvfb_run_found": False,
                        "xauth_found": False,
                        "xdotool_found": False,
                        "xwininfo_found": True,
                        "xwininfo_version": "xwininfo 1.3.4",
                        "xwininfo_path": "/usr/bin/xwininfo",
                        "import_found": False,
                        "wmctrl_found": False,
                    },
                ):
                    buf = io.StringIO()
                    with redirect_stdout(buf):
                        exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="ubuntu"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("install xvfb and xauth", output)
        self.assertIn("FAIL  git-lfs: installed at /home/daniel/.local/bin/git-lfs but unavailable to non-interactive shells", output)
        self.assertIn("FAIL  xdotool: missing; sudo apt-get install -y xdotool", output)
        self.assertIn("FAIL  import: missing; sudo apt-get install -y imagemagick", output)
        self.assertIn("WARN  wmctrl: missing; sudo apt-get install -y wmctrl", output)

    def test_cmd_desktop_doctor_reports_windows_ssh_handshake_reset(self):
        self._set_target_enabled("windows", True)
        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "ensure_windows_remote_repo_checkout",
                return_value={
                    "home_dir": r"C:\Users\danielraffel",
                    "repo_path": r"C:\Users\danielraffel\pulp-validate",
                    "repo_exists": True,
                    "git_dir_exists": True,
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

        with mock.patch.object(self.mod, "ssh_reachable", return_value=False):
            with mock.patch.object(
                self.mod,
                "ssh_failure_detail",
                return_value="win2 (SSH service reset during handshake; verify OpenSSH server on the target)",
            ):
                buf = io.StringIO()
                with redirect_stdout(buf):
                    exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="windows"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("FAIL  ssh: win2 (SSH service reset during handshake", output)

    def test_cmd_desktop_doctor_accepts_existing_linux_display_session(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="ubuntu"))

        with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
            with mock.patch.object(
                self.mod,
                "probe_linux_launch_backend",
                return_value={"mode": "display", "display": ":0", "xdg_runtime_dir": "/run/user/1000"},
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_linux_remote_tooling",
                    return_value={
                        "git_found": True,
                        "git_version": "git version 2.49.0",
                        "git_path": "/usr/bin/git",
                        "git_lfs_found": True,
                        "git_lfs_version": "git-lfs/3.5.1",
                        "git_lfs_path": "/usr/bin/git-lfs",
                        "xvfb_run_found": True,
                        "xvfb_run_version": "Usage: xvfb-run [OPTION ...] COMMAND",
                        "xvfb_run_path": "/usr/bin/xvfb-run",
                        "xauth_found": True,
                        "xauth_version": "1.1.2",
                        "xauth_path": "/usr/bin/xauth",
                        "xdotool_found": True,
                        "xdotool_version": "xdotool version 3.20211022.1",
                        "xdotool_path": "/usr/bin/xdotool",
                        "xwininfo_found": True,
                        "xwininfo_version": "xwininfo 1.3.4",
                        "xwininfo_path": "/usr/bin/xwininfo",
                        "import_found": True,
                        "import_version": "Version: ImageMagick 6.9.12-98",
                        "import_path": "/usr/bin/import",
                        "wmctrl_found": True,
                        "wmctrl_version": "1.07",
                        "wmctrl_path": "/usr/bin/wmctrl",
                    },
                ):
                    buf = io.StringIO()
                    with redirect_stdout(buf):
                        exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="ubuntu"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("PASS  launch_backend: existing display :0", output)

    def test_cmd_desktop_doctor_reports_windows_session_contract(self):
        session_probe = {
            "task_name": "PulpDesktopAutomationAgent-windows",
            "task_present": False,
            "task_state": "",
            "interactive_user": r"DESKTOP\daniel",
            "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
            "agent_root_exists": False,
            "jobs_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\jobs",
            "jobs_dir_exists": False,
            "results_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\results",
            "results_dir_exists": False,
            "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
            "script_exists": False,
        }
        self._install_windows_target(session_probe=session_probe)
        exit_code, output = self._run_windows_doctor(session_probe=session_probe)

        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop doctor for `windows`", output)
        self.assertIn("PASS  ssh: win", output)
        self.assertIn("WARN  scheduled_task: PulpDesktopAutomationAgent-windows (missing)", output)
        self.assertIn(r"PASS  interactive_user: DESKTOP\daniel", output)
        self.assertIn(r"PASS  git: git version 2.49.0.windows.1 (C:\Program Files\Git\cmd\git.exe)", output)
        self.assertIn("WARN  gh: missing; optional for remote GitHub workflows on the Windows target", output)
        self.assertIn(r"PASS  repo_checkout: C:\Users\danielraffel\pulp-validate (https://github.com/danielraffel/pulp)", output)

    def test_cmd_desktop_doctor_accepts_disconnected_windows_session(self):
        session_probe = {
            "task_name": "PulpDesktopAutomationAgent-windows",
            "task_present": True,
            "task_state": "Ready",
            "interactive_user": "",
            "logged_on_user": "danielraffel",
            "session_state": "Disc",
            "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
            "agent_root_exists": True,
            "jobs_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\jobs",
            "jobs_dir_exists": True,
            "results_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\results",
            "results_dir_exists": True,
            "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
            "script_exists": True,
        }
        self._install_windows_target(session_probe=session_probe)
        exit_code, output = self._run_windows_doctor(session_probe=session_probe)

        self.assertEqual(exit_code, 0)
        self.assertIn("PASS  interactive_user: danielraffel (Disc)", output)

    def test_cmd_desktop_doctor_treats_macos_accessibility_as_optional(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="mac"))

        with mock.patch.object(self.mod, "macos_accessibility_trusted", return_value=False):
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="mac"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("WARN  accessibility:", output)
        self.assertIn("Pulp app automation clicks still work without it", output)


if __name__ == "__main__":
    unittest.main()
