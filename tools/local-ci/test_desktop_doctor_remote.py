#!/usr/bin/env python3
"""No-network tests for remote desktop doctor check builders."""

from __future__ import annotations

import pathlib
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("desktop_doctor_remote.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


def check(name: str, ok: bool, detail: str, *, required: bool = True) -> dict:
    return {"name": name, "ok": ok, "detail": detail, "required": required}


class DesktopDoctorRemoteTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_ssh_desktop_doctor_checks_reports_missing_or_unreachable_host(self) -> None:
        missing = self.mod.ssh_desktop_doctor_checks(
            target_name="ubuntu",
            target={"adapter": "linux-xvfb", "bootstrap": "manual"},
            contract={},
            receipt=None,
            ssh_reachable_fn=lambda _host, _timeout: self.fail("missing host should not probe"),
            ssh_failure_detail_fn=lambda _host, _timeout: "unreachable",
            probe_linux_launch_backend_fn=lambda _host: {},
            probe_linux_remote_tooling_fn=lambda _host: {},
            probe_windows_session_agent_fn=lambda _host, _contract: {},
            probe_windows_remote_tooling_fn=lambda _host: {},
            probe_windows_repo_checkout_fn=lambda _host, _repo_path: {},
            desktop_check_fn=check,
        )
        self.assertEqual([item["name"] for item in missing], ["host", "bootstrap"])
        self.assertFalse(missing[0]["ok"])

        unreachable = self.mod.ssh_desktop_doctor_checks(
            target_name="ubuntu",
            target={"adapter": "linux-xvfb", "host": "ubuntu.example", "bootstrap": "xvfb-run"},
            contract={},
            receipt=None,
            ssh_reachable_fn=lambda _host, _timeout: False,
            ssh_failure_detail_fn=lambda host, timeout: f"{host} after {timeout}s",
            probe_linux_launch_backend_fn=lambda _host: self.fail("unreachable host should not run Linux probes"),
            probe_linux_remote_tooling_fn=lambda _host: {},
            probe_windows_session_agent_fn=lambda _host, _contract: {},
            probe_windows_remote_tooling_fn=lambda _host: {},
            probe_windows_repo_checkout_fn=lambda _host, _repo_path: {},
            desktop_check_fn=check,
        )
        by_name = {item["name"]: item for item in unreachable}
        self.assertFalse(by_name["ssh"]["ok"])
        self.assertEqual(by_name["ssh"]["detail"], "ubuntu.example after 5s")

    def test_linux_remote_doctor_checks_builds_backend_and_tooling_rows(self) -> None:
        checks = self.mod.linux_remote_doctor_checks(
            host="ubuntu.example",
            probe_linux_launch_backend_fn=lambda _host: {"mode": "display", "display": ":99"},
            probe_linux_remote_tooling_fn=lambda _host: {
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
            },
            desktop_check_fn=check,
        )
        by_name = {item["name"]: item for item in checks}

        self.assertTrue(by_name["launch_backend"]["ok"])
        self.assertEqual(by_name["launch_backend"]["detail"], "existing display :99")
        self.assertTrue(by_name["git"]["ok"])
        self.assertFalse(by_name["git-lfs"]["ok"])
        self.assertIn("sudo apt-get install -y git-lfs", by_name["git-lfs"]["detail"])
        self.assertFalse(by_name["wmctrl"]["required"])

    def test_linux_remote_doctor_checks_reports_probe_errors(self) -> None:
        checks = self.mod.linux_remote_doctor_checks(
            host="ubuntu.example",
            probe_linux_launch_backend_fn=lambda _host: (_ for _ in ()).throw(RuntimeError("backend failed")),
            probe_linux_remote_tooling_fn=lambda _host: (_ for _ in ()).throw(RuntimeError("tooling failed")),
            desktop_check_fn=check,
        )
        by_name = {item["name"]: item for item in checks}
        self.assertEqual(by_name["launch_backend"]["detail"], "backend failed")
        self.assertEqual(by_name["remote_tooling"]["detail"], "tooling failed")

    def test_windows_session_doctor_checks_builds_session_tooling_and_repo_rows(self) -> None:
        checks = self.mod.windows_session_doctor_checks(
            target_name="windows",
            target={"repo_path": r"C:\Users\dev"},
            contract={"task_name": "PulpDesktopAutomationAgent-windows", "remote_root": r"C:\agent"},
            receipt={"remote_bootstrap_ready": True},
            host="win.example",
            probe_windows_session_agent_fn=lambda _host, _contract: {
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
            },
            probe_windows_remote_tooling_fn=lambda _host: {
                "git_found": True,
                "git_path": r"C:\Program Files\Git\cmd\git.exe",
                "winget_found": False,
                "gh_found": True,
                "gh_path": r"C:\Program Files\GitHub CLI\gh.exe",
                "gh_auth_ready": False,
                "gh_auth_detail": "not logged in",
            },
            probe_windows_repo_checkout_fn=lambda _host, _repo_path: {
                "repo_path": r"C:\Users\dev",
                "repo_path_unsafe": True,
                "repo_exists": True,
                "git_dir_exists": True,
                "head_exists": True,
                "setup_exists": True,
            },
            desktop_check_fn=check,
        )
        by_name = {item["name"]: item for item in checks}

        self.assertTrue(by_name["scheduled_task"]["ok"])
        self.assertTrue(by_name["scheduled_task"]["required"])
        self.assertEqual(by_name["interactive_user"]["detail"], r"DEV\alice (Active)")
        self.assertFalse(by_name["jobs_dir"]["ok"])
        self.assertTrue(by_name["git"]["ok"])
        self.assertFalse(by_name["winget"]["required"])
        self.assertFalse(by_name["gh_auth"]["ok"])
        self.assertEqual(by_name["gh_auth"]["detail"], "not logged in")
        self.assertFalse(by_name["repo_checkout"]["ok"])
        self.assertIn("unsafe repo root", by_name["repo_checkout"]["detail"])

    def test_windows_session_doctor_checks_reports_probe_errors(self) -> None:
        checks = self.mod.windows_session_doctor_checks(
            target_name="windows",
            target={},
            contract={"task_name": "task"},
            receipt={"remote_bootstrap_ready": True},
            host="win.example",
            probe_windows_session_agent_fn=lambda _host, _contract: (_ for _ in ()).throw(RuntimeError("task failed")),
            probe_windows_remote_tooling_fn=lambda _host: {},
            probe_windows_repo_checkout_fn=lambda _host, _repo_path: {},
            desktop_check_fn=check,
        )
        by_name = {item["name"]: item for item in checks}
        self.assertEqual(by_name["scheduled_task"]["detail"], "task failed")
        self.assertTrue(by_name["scheduled_task"]["required"])


if __name__ == "__main__":
    unittest.main()
