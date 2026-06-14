#!/usr/bin/env python3
"""No-network tests for SSH remote desktop doctor dispatch."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_doctor_remote_ssh.py", add_module_dir=True)


def check(name: str, ok: bool, detail: str, *, required: bool = True) -> dict:
    return {"name": name, "ok": ok, "detail": detail, "required": required}


class DesktopDoctorRemoteSshTests(unittest.TestCase):
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

    def test_ssh_desktop_doctor_checks_dispatches_linux_and_windows_by_adapter(self) -> None:
        linux = self.mod.ssh_desktop_doctor_checks(
            target_name="ubuntu",
            target={"adapter": "linux-xvfb", "host": "ubuntu.example"},
            contract={},
            receipt=None,
            ssh_reachable_fn=lambda _host, _timeout: True,
            ssh_failure_detail_fn=lambda _host, _timeout: "unreachable",
            probe_linux_launch_backend_fn=lambda _host: {"mode": "xvfb", "path": "/usr/bin/xvfb-run"},
            probe_linux_remote_tooling_fn=lambda _host: {},
            probe_windows_session_agent_fn=lambda _host, _contract: self.fail("linux target should not run windows probes"),
            probe_windows_remote_tooling_fn=lambda _host: {},
            probe_windows_repo_checkout_fn=lambda _host, _repo_path: {},
            desktop_check_fn=check,
        )
        self.assertIn("launch_backend", {item["name"] for item in linux})

        windows = self.mod.ssh_desktop_doctor_checks(
            target_name="windows",
            target={"adapter": "windows-session-agent", "host": "win.example"},
            contract={"task_name": "task"},
            receipt=None,
            ssh_reachable_fn=lambda _host, _timeout: True,
            ssh_failure_detail_fn=lambda _host, _timeout: "unreachable",
            probe_linux_launch_backend_fn=lambda _host: self.fail("windows target should not run linux probes"),
            probe_linux_remote_tooling_fn=lambda _host: {},
            probe_windows_session_agent_fn=lambda _host, _contract: {"task_present": False},
            probe_windows_remote_tooling_fn=lambda _host: {},
            probe_windows_repo_checkout_fn=lambda _host, _repo_path: {},
            desktop_check_fn=check,
        )
        self.assertIn("scheduled_task", {item["name"] for item in windows})


if __name__ == "__main__":
    unittest.main()
