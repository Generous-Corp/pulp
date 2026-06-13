#!/usr/bin/env python3
"""Tests for target reachability compatibility facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("target_reachability_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetReachabilityBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_facade_reexports_ssh_utm_and_host_reachability_helpers(self) -> None:
        expected_exports = (
            "ssh_probe",
            "ssh_reachable",
            "ssh_failure_detail",
            "ssh_command_result",
            "utmctl_vm_status",
            "utmctl_start",
            "ensure_host_reachable",
            "preflight_target_host_state",
        )

        self.assertEqual(self.mod.TARGET_REACHABILITY_EXPORTS, expected_exports)
        for name in expected_exports:
            self.assertTrue(callable(getattr(self.mod, name)))


if __name__ == "__main__":
    unittest.main()
