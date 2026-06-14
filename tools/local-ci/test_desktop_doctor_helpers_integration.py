#!/usr/bin/env python3
"""Facade-level desktop doctor helper integration tests."""

from __future__ import annotations

import json
import pathlib
import tempfile
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(
        MODULE_PATH,
        module_name="pulp_local_ci_desktop_doctor_helpers_integration",
        add_module_dir=True,
    )


class DesktopDoctorHelpersIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_desktop_doctor_macos_local_optional_edges(self) -> None:
        config = {
            "desktop_automation": {
                "artifact_root": str(self.root / "artifacts"),
                "targets": {
                    "mac": {
                        "adapter": "macos-local",
                        "target_type": "local",
                        "optional": {
                            "webview_driver": True,
                            "debug_attach": True,
                            "video_capture": True,
                            "frame_stats": True,
                        },
                    }
                },
            }
        }
        which_values = {"screencapture": None, "osascript": "/usr/bin/osascript", "lldb": "/usr/bin/lldb", "ffmpeg": None}

        with mock.patch.object(self.mod.sys, "platform", "linux"):
            with mock.patch.object(self.mod, "desktop_receipt_for", return_value=None):
                with mock.patch.object(self.mod.shutil, "which", side_effect=lambda name: which_values.get(name)):
                    with mock.patch.object(
                        self.mod,
                        "macos_accessibility_trusted",
                        side_effect=json.JSONDecodeError("bad", "", 0),
                    ):
                        checks = {check["name"]: check for check in self.mod.desktop_doctor_checks(config, "mac")}

        self.assertTrue(checks["artifact_root"]["ok"])
        self.assertFalse(checks["receipt"]["ok"])
        self.assertIn("desktop install mac", checks["receipt"]["detail"])
        self.assertFalse(checks["platform"]["ok"])
        self.assertEqual(checks["platform"]["detail"], "running on linux")
        self.assertFalse(checks["screencapture"]["ok"])
        self.assertEqual(checks["screencapture"]["detail"], "missing")
        self.assertTrue(checks["osascript"]["ok"])
        self.assertFalse(checks["accessibility"]["ok"])
        self.assertFalse(checks["accessibility"]["required"])
        self.assertFalse(checks["webview_driver"]["ok"])
        self.assertIn("webdriver_url is not set", checks["webview_driver"]["detail"])
        self.assertTrue(checks["debug_attach"]["ok"])
        self.assertEqual(checks["debug_attach"]["detail"], "/usr/bin/lldb")
        self.assertFalse(checks["video_capture"]["ok"])
        self.assertIn("ffmpeg not found", checks["video_capture"]["detail"])
        self.assertTrue(checks["frame_stats"]["ok"])

    def test_desktop_doctor_remote_linux_and_windows_edges(self) -> None:
        config = {
            "desktop_automation": {
                "artifact_root": str(self.root / "artifacts"),
                "targets": {
                    "ubuntu": {
                        "adapter": "linux-xvfb",
                        "target_type": "ssh",
                        "host": "ubuntu.example",
                        "bootstrap": "xvfb-run",
                    },
                    "windows": {
                        "adapter": "windows-session-agent",
                        "target_type": "ssh",
                        "host": "win.example",
                        "repo_path": r"C:\Users\dev",
                        "bootstrap": "scheduled-task",
                    },
                },
            }
        }
        linux_tooling = {
            "git_found": True,
            "git_path": "/usr/bin/git",
            "git_lfs_found": False,
            "xvfb_run_found": True,
            "xvfb_run_path": "/usr/bin/xvfb-run",
            "xauth_found": True,
            "xdotool_found": True,
            "xwininfo_found": True,
            "import_found": True,
            "wmctrl_found": False,
        }
        windows_probe = {
            "task_present": True,
            "task_name": "PulpDesktopAutomationAgent-windows",
            "task_state": "Ready",
            "logged_on_user": " DEV\\alice ",
            "session_state": " Active ",
            "agent_root_exists": True,
            "remote_root": r"C:\agent",
            "jobs_dir_exists": False,
            "jobs_dir": r"C:\agent\jobs",
            "results_dir_exists": True,
            "results_dir": r"C:\agent\results",
            "script_exists": True,
            "script_path": r"C:\agent\agent.ps1",
        }
        windows_tooling = {
            "git_found": True,
            "git_path": r"C:\Program Files\Git\cmd\git.exe",
            "winget_found": False,
            "gh_found": True,
            "gh_path": r"C:\Program Files\GitHub CLI\gh.exe",
            "gh_auth_ready": False,
            "gh_auth_detail": "not logged in",
        }
        repo_probe = {
            "repo_path": r"C:\Users\dev",
            "repo_path_unsafe": True,
            "repo_exists": True,
            "git_dir_exists": True,
            "head_exists": True,
            "setup_exists": True,
        }

        with mock.patch.object(self.mod, "desktop_receipt_for", side_effect=[None, {"remote_bootstrap_ready": True}]):
            with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
                with mock.patch.object(self.mod, "ssh_failure_detail", return_value="unused"):
                    with mock.patch.object(
                        self.mod,
                        "probe_linux_launch_backend",
                        return_value={"mode": "display", "display": ":99"},
                    ):
                        with mock.patch.object(self.mod, "probe_linux_remote_tooling", return_value=linux_tooling):
                            linux_checks = {
                                check["name"]: check for check in self.mod.desktop_doctor_checks(config, "ubuntu")
                            }
                    with mock.patch.object(self.mod, "probe_windows_session_agent", return_value=windows_probe):
                        with mock.patch.object(self.mod, "probe_windows_remote_tooling", return_value=windows_tooling):
                            with mock.patch.object(self.mod, "probe_windows_repo_checkout", return_value=repo_probe):
                                windows_checks = {
                                    check["name"]: check
                                    for check in self.mod.desktop_doctor_checks(config, "windows")
                                }

        self.assertTrue(linux_checks["host"]["ok"])
        self.assertEqual(linux_checks["host"]["detail"], "ubuntu.example")
        self.assertTrue(linux_checks["ssh"]["ok"])
        self.assertTrue(linux_checks["launch_backend"]["ok"])
        self.assertIn("existing display :99", linux_checks["launch_backend"]["detail"])
        self.assertTrue(linux_checks["git"]["ok"])
        self.assertFalse(linux_checks["git-lfs"]["ok"])
        self.assertIn("sudo apt-get install -y git-lfs", linux_checks["git-lfs"]["detail"])
        self.assertFalse(linux_checks["wmctrl"]["required"])
        self.assertTrue(linux_checks["bootstrap"]["ok"])
        self.assertEqual(linux_checks["bootstrap"]["detail"], "xvfb-run")

        self.assertTrue(windows_checks["scheduled_task"]["ok"])
        self.assertTrue(windows_checks["scheduled_task"]["required"])
        self.assertEqual(windows_checks["interactive_user"]["detail"], r"DEV\alice (Active)")
        self.assertTrue(windows_checks["agent_root"]["ok"])
        self.assertFalse(windows_checks["jobs_dir"]["ok"])
        self.assertTrue(windows_checks["results_dir"]["ok"])
        self.assertTrue(windows_checks["script_path"]["ok"])
        self.assertTrue(windows_checks["git"]["ok"])
        self.assertFalse(windows_checks["winget"]["ok"])
        self.assertFalse(windows_checks["winget"]["required"])
        self.assertTrue(windows_checks["gh"]["ok"])
        self.assertFalse(windows_checks["gh_auth"]["ok"])
        self.assertEqual(windows_checks["gh_auth"]["detail"], "not logged in")
        self.assertFalse(windows_checks["repo_checkout"]["ok"])
        self.assertIn("unsafe repo root", windows_checks["repo_checkout"]["detail"])
        self.assertEqual(windows_checks["bootstrap"]["detail"], "scheduled-task")


if __name__ == "__main__":
    unittest.main()
