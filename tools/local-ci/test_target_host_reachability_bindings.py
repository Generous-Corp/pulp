#!/usr/bin/env python3
"""Tests for host reachability orchestration dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("target_host_reachability_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetHostReachabilityBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_host_reachability_bindings_delegate_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        preflight = types.SimpleNamespace(
            ensure_host_reachable=capture("ensure", "host"),
            preflight_target_host_state=capture("host_state", {"status": "local"}),
        )
        bindings = {
            "_target_preflight": preflight,
            "ssh_reachable": object(),
            "utmctl_vm_status": object(),
            "utmctl_start": object(),
            "time": types.SimpleNamespace(time=object(), sleep=object()),
            "print": object(),
        }

        self.assertEqual(self.mod.ensure_host_reachable(bindings, "ubuntu", {"host": "h"}, {}), "host")
        self.assertIs(captured["ensure"][1]["ssh_reachable_fn"], bindings["ssh_reachable"])
        self.assertIs(captured["ensure"][1]["utmctl_vm_status_fn"], bindings["utmctl_vm_status"])
        self.assertIs(captured["ensure"][1]["utmctl_start_fn"], bindings["utmctl_start"])
        self.assertIs(captured["ensure"][1]["time_fn"], bindings["time"].time)
        self.assertIs(captured["ensure"][1]["sleep_fn"], bindings["time"].sleep)
        self.assertIs(captured["ensure"][1]["print_fn"], bindings["print"])

        self.assertEqual(self.mod.preflight_target_host_state(bindings, "mac", {"type": "local"}, {}), {"status": "local"})
        self.assertIs(captured["host_state"][1]["ssh_reachable_fn"], bindings["ssh_reachable"])


if __name__ == "__main__":
    unittest.main()
