#!/usr/bin/env python3
"""Tests for scalar normalization dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock
from pathlib import Path



def load_module():
    return load_local_ci_module("normalize_scalar_bindings.py")


class NormalizeScalarBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        normalize = types.SimpleNamespace(
            PRIORITY_VALUES={"normal": 50, "high": 75},
            normalize_priority=make_runner("normalize_priority", "normal"),
            priority_value=make_runner("priority_value", 50),
            normalize_validation_mode=make_runner("normalize_validation_mode", "full"),
            normalize_desktop_source_mode=make_runner("normalize_desktop_source_mode", "live"),
            default_desktop_artifact_root=make_runner("default_desktop_artifact_root", Path("/runs")),
            normalize_publish_mode=make_runner("normalize_publish_mode", "branch"),
            parse_config_bool=make_runner("parse_config_bool", True),
        )
        return {"_normalize": normalize}, calls

    def test_priority_values_delegates_to_normalize_module_constant(self):
        bindings, _calls = self._bindings()

        self.assertEqual(self.mod.priority_values(bindings), {"normal": 50, "high": 75})

    def test_scalar_normalizers_delegate_to_normalize_module(self):
        bindings, calls = self._bindings()

        self.assertEqual(self.mod.normalize_priority(bindings, "NORMAL"), "normal")
        self.assertEqual(self.mod.priority_value(bindings, "normal"), 50)
        self.assertEqual(self.mod.normalize_validation_mode(bindings, "FULL"), "full")
        self.assertEqual(self.mod.normalize_desktop_source_mode(bindings, "live"), "live")
        self.assertEqual(self.mod.default_desktop_artifact_root(bindings), Path("/runs"))
        self.assertEqual(self.mod.normalize_publish_mode(bindings, "branch"), "branch")
        self.assertTrue(self.mod.parse_config_bool(bindings, "yes"))

        self.assertEqual(
            [call[0] for call in calls],
            [
                "normalize_priority",
                "priority_value",
                "normalize_validation_mode",
                "normalize_desktop_source_mode",
                "default_desktop_artifact_root",
                "normalize_publish_mode",
                "parse_config_bool",
            ],
        )
        self.assertEqual(calls[0][1], ("NORMAL",))
        self.assertEqual(calls[4][1], ())

    def test_install_normalize_scalar_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_normalize_scalar_helpers(bindings, ("normalize_priority", "custom_scalar"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("normalize_priority",)),
                mock.call(bindings, self.mod.__dict__, ("custom_scalar",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
