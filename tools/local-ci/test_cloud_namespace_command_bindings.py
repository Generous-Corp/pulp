#!/usr/bin/env python3
"""Tests for cloud Namespace command bindings."""

import types
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("cloud_namespace_command_bindings.py")


class CloudNamespaceCommandBindingsTests(unittest.TestCase):
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
            cmd_cloud_namespace_doctor=make_runner("cmd_cloud_namespace_doctor", 17),
            cmd_cloud_namespace_setup=make_runner("cmd_cloud_namespace_setup", 18),
        )
        return {"_cloud": cloud}, calls

    def test_namespace_command_exports_match_wrappers(self):
        expected = (
            "cmd_cloud_namespace_doctor",
            "cmd_cloud_namespace_setup",
        )

        self.assertEqual(self.mod.CLOUD_NAMESPACE_COMMAND_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_namespace_commands_delegate_to_cloud_module(self):
        bindings, calls = self._bindings()
        args = object()
        cases = [
            ("cmd_cloud_namespace_doctor", 17),
            ("cmd_cloud_namespace_setup", 18),
        ]

        for name, expected in cases:
            with self.subTest(name=name):
                self.assertEqual(getattr(self.mod, name)(bindings, args), expected)

        self.assertEqual([call[0] for call in calls], [name for name, _ in cases])
        for _name, call_args, call_kwargs in calls:
            self.assertEqual(call_args, (args,))
            self.assertEqual(call_kwargs, {})

    def test_install_namespace_helpers_wires_named_exports(self):
        bindings, calls = self._bindings()
        self.mod.install_cloud_namespace_command_helpers(bindings, ("cmd_cloud_namespace_setup",))
        args = object()

        self.assertEqual(bindings["cmd_cloud_namespace_setup"](args), 18)
        self.assertEqual(calls, [("cmd_cloud_namespace_setup", (args,), {})])
        self.assertEqual(bindings["cmd_cloud_namespace_setup"].__name__, "cmd_cloud_namespace_setup")


if __name__ == "__main__":
    unittest.main()
