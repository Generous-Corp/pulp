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

    def test_host_reachability_exports_are_declared(self) -> None:
        self.assertEqual(
            self.mod.TARGET_HOST_REACHABILITY_EXPORTS,
            ("ensure_host_reachable", "preflight_target_host_state"),
        )

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

    def test_install_target_host_reachability_helpers_wires_named_exports(self) -> None:
        preflight = types.SimpleNamespace(
            preflight_target_host_state=lambda target_name, target_cfg, defaults, **kwargs: {"target": target_name}
        )
        bindings = {
            "_target_preflight": preflight,
            "ssh_reachable": object(),
        }

        self.mod.install_target_host_reachability_helpers(bindings, ("preflight_target_host_state",))

        self.assertEqual(bindings["preflight_target_host_state"]("mac", {}, {}), {"target": "mac"})
        self.assertEqual(bindings["preflight_target_host_state"].__name__, "preflight_target_host_state")
        self.assertNotIn("ensure_host_reachable", bindings)

    def test_install_target_host_reachability_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_target_host_reachability_helper = lambda _bindings: "future"

        self.mod.install_target_host_reachability_helpers(bindings, ("future_target_host_reachability_helper",))

        self.assertEqual(bindings["future_target_host_reachability_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
