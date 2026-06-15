#!/usr/bin/env python3
"""Tests for SSH target reachability dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("target_ssh_reachability_bindings.py")


class TargetSshReachabilityBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_ssh_reachability_exports_are_declared(self) -> None:
        self.assertEqual(
            self.mod.TARGET_SSH_REACHABILITY_EXPORTS,
            (
                "ssh_probe",
                "ssh_reachable",
                "ssh_failure_detail",
                "ssh_command_result",
            ),
        )

    def test_ssh_bindings_delegate_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        preflight = types.SimpleNamespace(
            ssh_probe=capture("ssh_probe", {"ok": True}),
            ssh_reachable=capture("ssh_reachable", True),
            ssh_failure_detail=capture("ssh_failure", "detail"),
            ssh_command_result=capture("ssh_command", {"run": True}),
        )
        bindings = {
            "_target_preflight": preflight,
            "run_ssh_subprocess": object(),
            "ssh_probe": object(),
        }

        self.assertEqual(self.mod.ssh_probe(bindings, "host", 9), {"ok": True})
        self.assertIs(captured["ssh_probe"][1]["run_ssh_subprocess_fn"], bindings["run_ssh_subprocess"])
        self.assertTrue(self.mod.ssh_reachable(bindings, "host", 9))
        self.assertIs(captured["ssh_reachable"][1]["ssh_probe_fn"], bindings["ssh_probe"])
        self.assertEqual(self.mod.ssh_failure_detail(bindings, "host", 9), "detail")
        self.assertIs(captured["ssh_failure"][1]["ssh_probe_fn"], bindings["ssh_probe"])
        self.assertEqual(self.mod.ssh_command_result(bindings, "host", "echo ok", timeout=44), {"run": True})
        self.assertIs(captured["ssh_command"][1]["run_ssh_subprocess_fn"], bindings["run_ssh_subprocess"])

    def test_install_target_ssh_reachability_helpers_wires_named_exports(self) -> None:
        preflight = types.SimpleNamespace(ssh_probe=lambda host, timeout, **kwargs: {"host": host, "timeout": timeout})
        bindings = {
            "_target_preflight": preflight,
            "run_ssh_subprocess": object(),
        }

        self.mod.install_target_ssh_reachability_helpers(bindings, ("ssh_probe",))

        self.assertEqual(bindings["ssh_probe"]("host", 9), {"host": "host", "timeout": 9})
        self.assertEqual(bindings["ssh_probe"].__name__, "ssh_probe")
        self.assertNotIn("ssh_reachable", bindings)


if __name__ == "__main__":
    unittest.main()
