#!/usr/bin/env python3
"""Tests for cloud run/status command bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("cloud_run_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudRunCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        cloud = types.SimpleNamespace(
            cmd_cloud_run=make_runner("cmd_cloud_run", 15),
            cmd_cloud_status=make_runner("cmd_cloud_status", 16),
        )
        return {"_cloud": cloud}, calls

    def test_run_command_exports_match_wrappers(self):
        expected = (
            "cmd_cloud_run",
            "cmd_cloud_status",
        )

        self.assertEqual(self.mod.CLOUD_RUN_COMMAND_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_run_commands_delegate_to_cloud_module(self):
        bindings, calls = self._bindings()
        args = object()
        cases = [
            ("cmd_cloud_run", 15),
            ("cmd_cloud_status", 16),
        ]

        for name, expected in cases:
            with self.subTest(name=name):
                self.assertEqual(getattr(self.mod, name)(bindings, args), expected)

        self.assertEqual([call[0] for call in calls], [name for name, _ in cases])
        for _name, call_args, call_kwargs in calls:
            self.assertEqual(call_args, (args,))
            self.assertEqual(call_kwargs, {})

    def test_install_run_helpers_wires_named_exports(self):
        bindings, calls = self._bindings()
        self.mod.install_cloud_run_command_helpers(bindings, ("cmd_cloud_run",))
        args = object()

        self.assertEqual(bindings["cmd_cloud_run"](args), 15)
        self.assertEqual(calls, [("cmd_cloud_run", (args,), {})])
        self.assertEqual(bindings["cmd_cloud_run"].__name__, "cmd_cloud_run")

    def test_install_cloud_run_command_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_cloud_run_command_helper = lambda _bindings: "future"

        self.mod.install_cloud_run_command_helpers(bindings, ("future_cloud_run_command_helper",))

        self.assertEqual(bindings["future_cloud_run_command_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
