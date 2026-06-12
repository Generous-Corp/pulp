#!/usr/bin/env python3
"""Tests for target selection facade bindings."""

import importlib.util
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("target_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("target_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class TargetBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_target_helpers_delegate_to_targets_module(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        bindings = {
            "_targets": types.SimpleNamespace(
                enabled_targets=make_runner("enabled_targets", ["mac"]),
                parse_targets_arg=make_runner("parse_targets_arg", ["mac", "windows"]),
                resolve_targets=make_runner("resolve_targets", ["mac"]),
            )
        }
        config = {"targets": {"mac": {"enabled": True}}}
        requested = ["mac"]

        self.assertEqual(self.mod.enabled_targets(bindings, config), ["mac"])
        self.assertEqual(self.mod.parse_targets_arg(bindings, "mac,windows"), ["mac", "windows"])
        self.assertEqual(self.mod.resolve_targets(bindings, config, requested), ["mac"])

        self.assertEqual([call[0] for call in calls], [
            "enabled_targets",
            "parse_targets_arg",
            "resolve_targets",
        ])
        self.assertEqual(calls[0][1], (config,))
        self.assertEqual(calls[1][1], ("mac,windows",))
        self.assertEqual(calls[2][1], (config, requested))


if __name__ == "__main__":
    unittest.main()
