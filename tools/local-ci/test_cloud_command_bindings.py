#!/usr/bin/env python3
"""Tests for cloud command facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("cloud_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_command_exports_match_wrappers(self):
        expected = (
            "cmd_cloud_workflows",
            "cmd_cloud_defaults",
            "cmd_cloud_history",
            "cmd_cloud_compare",
            "cmd_cloud_recommend",
            "cmd_cloud_run",
            "cmd_cloud_status",
            "cmd_cloud_namespace_doctor",
            "cmd_cloud_namespace_setup",
        )

        self.assertEqual(self.mod.CLOUD_COMMAND_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_cloud_commands_delegate_to_cloud_module(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        cloud = types.SimpleNamespace(
            cmd_cloud_workflows=make_runner("cmd_cloud_workflows", 10),
            cmd_cloud_defaults=make_runner("cmd_cloud_defaults", 11),
            cmd_cloud_history=make_runner("cmd_cloud_history", 12),
            cmd_cloud_compare=make_runner("cmd_cloud_compare", 13),
            cmd_cloud_recommend=make_runner("cmd_cloud_recommend", 14),
            cmd_cloud_run=make_runner("cmd_cloud_run", 15),
            cmd_cloud_status=make_runner("cmd_cloud_status", 16),
            cmd_cloud_namespace_doctor=make_runner("cmd_cloud_namespace_doctor", 17),
            cmd_cloud_namespace_setup=make_runner("cmd_cloud_namespace_setup", 18),
        )
        bindings = {"_cloud": cloud}
        args = object()

        cases = [
            ("cmd_cloud_workflows", 10),
            ("cmd_cloud_defaults", 11),
            ("cmd_cloud_history", 12),
            ("cmd_cloud_compare", 13),
            ("cmd_cloud_recommend", 14),
            ("cmd_cloud_run", 15),
            ("cmd_cloud_status", 16),
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

    def test_install_cloud_command_helpers_wires_named_exports(self):
        calls = []
        cloud = types.SimpleNamespace(cmd_cloud_run=lambda args: calls.append(("cmd_cloud_run", args)) or 15)
        bindings = {"_cloud": cloud}

        self.mod.install_cloud_command_helpers(bindings, ("cmd_cloud_run",))

        args = object()
        self.assertEqual(bindings["cmd_cloud_run"](args), 15)
        self.assertEqual(calls, [("cmd_cloud_run", args)])


if __name__ == "__main__":
    unittest.main()
