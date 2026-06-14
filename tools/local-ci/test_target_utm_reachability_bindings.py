#!/usr/bin/env python3
"""Tests for UTM target reachability dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("target_utm_reachability_bindings.py")


class TargetUtmReachabilityBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_utm_reachability_exports_are_declared(self) -> None:
        self.assertEqual(
            self.mod.TARGET_UTM_REACHABILITY_EXPORTS,
            ("utmctl_vm_status", "utmctl_start"),
        )

    def test_utm_bindings_delegate_subprocess_run_dependency(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        preflight = types.SimpleNamespace(
            utmctl_vm_status=capture("utm_status", "started"),
            utmctl_start=capture("utm_start", True),
        )
        bindings = {
            "_target_preflight": preflight,
            "subprocess": types.SimpleNamespace(run=object()),
        }

        self.assertEqual(self.mod.utmctl_vm_status(bindings, "VM"), "started")
        self.assertIs(captured["utm_status"][1]["run_fn"], bindings["subprocess"].run)
        self.assertTrue(self.mod.utmctl_start(bindings, "VM"))
        self.assertIs(captured["utm_start"][1]["run_fn"], bindings["subprocess"].run)

    def test_install_target_utm_reachability_helpers_wires_named_exports(self) -> None:
        preflight = types.SimpleNamespace(utmctl_vm_status=lambda vm_name, **kwargs: "started")
        bindings = {
            "_target_preflight": preflight,
            "subprocess": types.SimpleNamespace(run=object()),
        }

        self.mod.install_target_utm_reachability_helpers(bindings, ("utmctl_vm_status",))

        self.assertEqual(bindings["utmctl_vm_status"]("VM"), "started")
        self.assertEqual(bindings["utmctl_vm_status"].__name__, "utmctl_vm_status")
        self.assertNotIn("utmctl_start", bindings)


if __name__ == "__main__":
    unittest.main()
