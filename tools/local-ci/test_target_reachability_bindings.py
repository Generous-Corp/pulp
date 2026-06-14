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
            *self.mod.TARGET_SSH_REACHABILITY_EXPORTS,
            *self.mod.TARGET_UTM_REACHABILITY_EXPORTS,
            *self.mod.TARGET_HOST_REACHABILITY_EXPORTS,
        )

        self.assertEqual(self.mod.TARGET_REACHABILITY_EXPORTS, expected_exports)
        self.assertEqual(len(expected_exports), len(set(expected_exports)))
        for name in expected_exports:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_target_reachability_helpers_routes_each_group(self) -> None:
        calls = []

        def ssh_install(bindings, names):
            calls.append(("ssh", names))

        def utm_install(bindings, names):
            calls.append(("utm", names))

        def host_install(bindings, names):
            calls.append(("host", names))

        self.mod.install_target_ssh_reachability_helpers = ssh_install
        self.mod.install_target_utm_reachability_helpers = utm_install
        self.mod.install_target_host_reachability_helpers = host_install

        self.mod.install_target_reachability_helpers(
            {},
            ("ssh_probe", "utmctl_start", "preflight_target_host_state"),
        )

        self.assertEqual(
            calls,
            [
                ("ssh", ("ssh_probe",)),
                ("utm", ("utmctl_start",)),
                ("host", ("preflight_target_host_state",)),
            ],
        )

    def test_install_target_reachability_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_target_reachability_helper = lambda _bindings: "future"

        self.mod.install_target_reachability_helpers(bindings, ("future_target_reachability_helper",))

        self.assertEqual(bindings["future_target_reachability_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
