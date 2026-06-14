#!/usr/bin/env python3
"""Tests for target reachability compatibility facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("target_reachability_bindings.py")


class TargetReachabilityBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_facade_reexports_ssh_utm_and_host_reachability_helpers(self) -> None:
        expected_exports = (
            *self.mod.TARGET_SSH_REACHABILITY_EXPORTS,
            *self.mod.TARGET_UTM_REACHABILITY_EXPORTS,
            *self.mod.TARGET_HOST_REACHABILITY_EXPORTS,
        )

        self.assertEqual(self.mod.TARGET_REACHABILITY_EXPORTS, expected_exports)
        self.assertEqual(len(expected_exports), len(set(expected_exports)))
        for name in expected_exports:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_target_reachability_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_target_ssh_reachability_helpers") as ssh,
            mock.patch.object(self.mod, "install_target_utm_reachability_helpers") as utm,
            mock.patch.object(self.mod, "install_target_host_reachability_helpers") as host,
        ):
            self.mod.install_target_reachability_helpers(
                bindings,
                ("ssh_probe", "utmctl_start", "preflight_target_host_state"),
            )

        ssh.assert_called_once_with(bindings, ("ssh_probe",))
        utm.assert_called_once_with(bindings, ("utmctl_start",))
        host.assert_called_once_with(bindings, ("preflight_target_host_state",))


if __name__ == "__main__":
    unittest.main()
