#!/usr/bin/env python3
"""Tests for direct cloud billing module-attribute binding installer."""

from __future__ import annotations

import types
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("cloud_billing_attr_bindings.py")


class CloudBillingAttrBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_billing_exports_are_unique(self):
        self.assertIn("estimate_cloud_record_cost", self.mod.CLOUD_BILLING_EXPORTS)
        self.assertIn("summarize_namespace_usage", self.mod.CLOUD_BILLING_EXPORTS)
        self.assertEqual(len(self.mod.CLOUD_BILLING_EXPORTS), len(set(self.mod.CLOUD_BILLING_EXPORTS)))

    def test_install_cloud_billing_attr_helpers_wires_late_bound_exports(self):
        calls = []

        def estimate_cloud_record_cost(record, settings):
            calls.append(("estimate_cloud_record_cost", record, settings))
            return 1.25

        bindings = {"_cloud": types.SimpleNamespace(estimate_cloud_record_cost=estimate_cloud_record_cost)}

        self.mod.install_cloud_billing_attr_helpers(bindings, ("estimate_cloud_record_cost",))

        self.assertEqual(bindings["estimate_cloud_record_cost"]({"provider": "github"}, {"rates": {}}), 1.25)
        self.assertEqual(calls, [("estimate_cloud_record_cost", {"provider": "github"}, {"rates": {}})])


if __name__ == "__main__":
    unittest.main()
