#!/usr/bin/env python3
"""No-network tests for UTM target reachability helpers."""

from __future__ import annotations

import subprocess
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("target_utm_reachability.py")


class TargetUtmReachabilityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_status_and_start_use_injected_runner(self) -> None:
        def fake_run(command, **_kwargs):
            if command == ["utmctl", "list"]:
                return subprocess.CompletedProcess(command, 0, stdout="Ubuntu stopped\nWindows started\n", stderr="")
            return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

        self.assertEqual(self.mod.utmctl_vm_status("Ubuntu", run_fn=fake_run), "stopped")
        self.assertIsNone(
            self.mod.utmctl_vm_status(
                "Missing",
                run_fn=lambda command, **_kwargs: subprocess.CompletedProcess(command, 0, stdout="", stderr=""),
            )
        )
        self.assertTrue(self.mod.utmctl_start("Ubuntu", run_fn=fake_run))


if __name__ == "__main__":
    unittest.main()
