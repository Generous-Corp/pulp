#!/usr/bin/env python3
"""Tests for UTM target reachability dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("target_utm_reachability_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetUtmReachabilityBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

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


if __name__ == "__main__":
    unittest.main()
