#!/usr/bin/env python3
"""Tests for target reachability facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("target_reachability_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetReachabilityBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, preflight):
        bindings = {
            "_target_preflight": preflight,
            "subprocess": types.SimpleNamespace(run=object()),
            "time": types.SimpleNamespace(time=object(), sleep=object()),
            "print": object(),
        }
        for name in [
            "run_ssh_subprocess",
            "ssh_probe",
            "ssh_reachable",
            "utmctl_vm_status",
            "utmctl_start",
        ]:
            bindings[name] = object()
        return bindings

    def test_ssh_and_utm_bindings_delegate_facade_dependencies(self) -> None:
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
            utmctl_vm_status=capture("utm_status", "started"),
            utmctl_start=capture("utm_start", True),
            ensure_host_reachable=capture("ensure", "host"),
            preflight_target_host_state=capture("host_state", {"status": "local"}),
        )
        bindings = self._bindings(preflight)

        self.assertEqual(self.mod.ssh_probe(bindings, "host", 9), {"ok": True})
        self.assertIs(captured["ssh_probe"][1]["run_ssh_subprocess_fn"], bindings["run_ssh_subprocess"])
        self.assertTrue(self.mod.ssh_reachable(bindings, "host", 9))
        self.assertIs(captured["ssh_reachable"][1]["ssh_probe_fn"], bindings["ssh_probe"])
        self.assertEqual(self.mod.ssh_failure_detail(bindings, "host", 9), "detail")
        self.assertEqual(self.mod.ssh_command_result(bindings, "host", "echo ok", timeout=44), {"run": True})
        self.assertIs(captured["ssh_command"][1]["run_ssh_subprocess_fn"], bindings["run_ssh_subprocess"])
        self.assertEqual(self.mod.utmctl_vm_status(bindings, "VM"), "started")
        self.assertIs(captured["utm_status"][1]["run_fn"], bindings["subprocess"].run)
        self.assertTrue(self.mod.utmctl_start(bindings, "VM"))
        self.assertIs(captured["utm_start"][1]["run_fn"], bindings["subprocess"].run)

        self.assertEqual(self.mod.ensure_host_reachable(bindings, "ubuntu", {"host": "h"}, {}), "host")
        self.assertIs(captured["ensure"][1]["ssh_reachable_fn"], bindings["ssh_reachable"])
        self.assertIs(captured["ensure"][1]["time_fn"], bindings["time"].time)
        self.assertIs(captured["ensure"][1]["sleep_fn"], bindings["time"].sleep)
        self.assertIs(captured["ensure"][1]["print_fn"], bindings["print"])

        self.assertEqual(self.mod.preflight_target_host_state(bindings, "mac", {"type": "local"}, {}), {"status": "local"})
        self.assertIs(captured["host_state"][1]["ssh_reachable_fn"], bindings["ssh_reachable"])


if __name__ == "__main__":
    unittest.main()
