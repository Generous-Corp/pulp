#!/usr/bin/env python3
"""No-network tests for local-ci target reachability helpers."""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("target_reachability.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetReachabilityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_ssh_helpers_use_injected_runner(self) -> None:
        calls: list[tuple[list[str], dict]] = []

        def fake_run_ssh(command, **kwargs):
            calls.append((command, kwargs))
            return subprocess.CompletedProcess(command, 0, stdout="up\n", stderr="")

        result = self.mod.ssh_probe("ubuntu", 2, run_ssh_subprocess_fn=fake_run_ssh)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(calls[-1][0], ["ssh", "-o", "ConnectTimeout=2", "-o", "BatchMode=yes", "ubuntu", "echo", "up"])
        self.assertEqual(calls[-1][1]["timeout"], 5)
        self.assertTrue(self.mod.ssh_reachable("ubuntu", 5, ssh_probe_fn=lambda _host, _timeout: result))

        def timeout_run_ssh(command, **_kwargs):
            raise subprocess.TimeoutExpired(command, 5)

        timed_out = self.mod.ssh_probe("win2", 5, run_ssh_subprocess_fn=timeout_run_ssh)
        self.assertEqual(timed_out.returncode, 124)
        self.assertIn("timed out", timed_out.stderr.lower())

        reset = subprocess.CompletedProcess(["ssh"], 255, "", "kex_exchange_identification: read: Connection reset by peer\n")
        detail = self.mod.ssh_failure_detail("win2", 5, ssh_probe_fn=lambda _host, _timeout: reset)
        self.assertEqual(detail, "win2 (SSH service reset during handshake; verify OpenSSH server on the target)")

        self.mod.ssh_command_result("ubuntu", "echo ok", timeout=90, run_ssh_subprocess_fn=fake_run_ssh)
        self.assertEqual(calls[-1][0][:4], ["ssh", "-o", "ConnectTimeout=30", "ubuntu"])
        self.assertEqual(calls[-1][1]["timeout"], 90)
        self.assertIn('export PATH="$HOME/.local/bin:$PATH"; echo ok', calls[-1][0][-1])

    def test_utm_status_and_reachability_fallbacks_are_injected(self) -> None:
        def fake_run(command, **_kwargs):
            if command == ["utmctl", "list"]:
                return subprocess.CompletedProcess(command, 0, stdout="Ubuntu stopped\nWindows started\n", stderr="")
            return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

        self.assertEqual(self.mod.utmctl_vm_status("Ubuntu", run_fn=fake_run), "stopped")
        self.assertTrue(self.mod.utmctl_start("Ubuntu", run_fn=fake_run))

        reachable_hosts: list[str] = []

        def reachable(host, _timeout):
            reachable_hosts.append(host)
            return host == "fallback"

        logs: list[str] = []
        resolved = self.mod.ensure_host_reachable(
            "ubuntu",
            {"host": "primary", "fallback_host": "fallback"},
            {},
            ssh_reachable_fn=reachable,
            utmctl_vm_status_fn=lambda _name: None,
            utmctl_start_fn=lambda _name: False,
            time_fn=lambda: 0,
            sleep_fn=lambda _secs: None,
            print_fn=logs.append,
        )
        self.assertEqual(resolved, "fallback")
        self.assertEqual(reachable_hosts, ["primary", "fallback"])
        self.assertTrue(any("trying fallback" in line for line in logs))

        start_calls: list[str] = []
        sleep_calls: list[float] = []
        probe_count = {"count": 0}

        def reachable_after_start(_host, _timeout):
            probe_count["count"] += 1
            return probe_count["count"] > 1

        resolved_after_start = self.mod.ensure_host_reachable(
            "ubuntu",
            {"host": "primary", "utm_fallback": {"vm_name": "Ubuntu", "boot_wait_secs": 0, "ssh_retry_secs": 10}},
            {},
            ssh_reachable_fn=reachable_after_start,
            utmctl_vm_status_fn=lambda _name: "stopped",
            utmctl_start_fn=lambda name: start_calls.append(name) or True,
            time_fn=iter([0, 1]).__next__,
            sleep_fn=sleep_calls.append,
            print_fn=lambda *_args, **_kwargs: None,
        )
        self.assertEqual(resolved_after_start, "primary")
        self.assertEqual(start_calls, ["Ubuntu"])
        self.assertEqual(sleep_calls, [0])

    def test_preflight_target_host_state_reports_local_fallback_utm_and_unreachable(self) -> None:
        self.assertEqual(
            self.mod.preflight_target_host_state("mac", {"type": "local"}, {}, ssh_reachable_fn=lambda *_args: False),
            {"target": "mac", "transport_mode": "local", "status": "local"},
        )

        fallback_state = self.mod.preflight_target_host_state(
            "windows",
            {"type": "ssh", "host": "win2", "fallback_host": "win", "repo_path": "C:\\Pulp"},
            {},
            ssh_reachable_fn=lambda host, _timeout: host == "win",
        )
        self.assertEqual(fallback_state["status"], "fallback-up")
        self.assertEqual(fallback_state["resolved_host"], "win")
        self.assertIn("fallback win is up", fallback_state["warning"])

        utm_state = self.mod.preflight_target_host_state(
            "ubuntu",
            {"type": "ssh", "host": "ubuntu", "utm_fallback": {"vm_name": "Ubuntu"}},
            {},
            ssh_reachable_fn=lambda *_args: False,
        )
        self.assertEqual(utm_state["status"], "utm-fallback-pending")
        self.assertIn("UTM fallback 'Ubuntu'", utm_state["warning"])

        unreachable = self.mod.preflight_target_host_state(
            "ubuntu",
            {"type": "ssh", "host": "ubuntu"},
            {},
            ssh_reachable_fn=lambda *_args: False,
        )
        self.assertEqual(unreachable["status"], "unreachable")
        self.assertIn("no fallback host or UTM VM", unreachable["error"])


if __name__ == "__main__":
    unittest.main()
