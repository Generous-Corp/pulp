#!/usr/bin/env python3
"""No-network tests for SSH target reachability helpers."""

from __future__ import annotations

import subprocess
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("target_ssh_reachability.py")


class TargetSshReachabilityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_reachable_failure_detail_and_command_use_injected_runner(self) -> None:
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


if __name__ == "__main__":
    unittest.main()
