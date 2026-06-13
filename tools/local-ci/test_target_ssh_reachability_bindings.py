#!/usr/bin/env python3
"""Tests for SSH target reachability dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("target_ssh_reachability_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetSshReachabilityBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

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


if __name__ == "__main__":
    unittest.main()
