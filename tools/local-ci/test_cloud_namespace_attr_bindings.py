#!/usr/bin/env python3
"""Tests for direct cloud Namespace module-attribute binding installer."""

from __future__ import annotations

import types
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("cloud_namespace_attr_bindings.py")


class CloudNamespaceAttrBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_namespace_exports_are_unique(self):
        self.assertIn("nsc_available", self.mod.CLOUD_NAMESPACE_EXPORTS)
        self.assertIn("print_namespace_setup_help", self.mod.CLOUD_NAMESPACE_EXPORTS)
        self.assertEqual(len(self.mod.CLOUD_NAMESPACE_EXPORTS), len(set(self.mod.CLOUD_NAMESPACE_EXPORTS)))

    def test_install_cloud_namespace_attr_helpers_wires_late_bound_exports(self):
        calls = []

        def nsc_available():
            calls.append("nsc_available")
            return True

        bindings = {"_cloud": types.SimpleNamespace(nsc_available=nsc_available)}

        self.mod.install_cloud_namespace_attr_helpers(bindings, ("nsc_available",))

        self.assertTrue(bindings["nsc_available"]())
        self.assertEqual(calls, ["nsc_available"])


if __name__ == "__main__":
    unittest.main()
