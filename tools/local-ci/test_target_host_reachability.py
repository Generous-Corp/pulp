#!/usr/bin/env python3
"""No-network tests for target host reachability orchestration."""

from __future__ import annotations

import unittest
from pathlib import Path

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("target_host_reachability.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetHostReachabilityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_reachability_resolves_fallback_and_utm_start(self) -> None:
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
