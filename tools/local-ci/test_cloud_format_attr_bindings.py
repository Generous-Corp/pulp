#!/usr/bin/env python3
"""Tests for direct cloud formatting module-attribute binding installer."""

from __future__ import annotations

import types
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("cloud_format_attr_bindings.py")


class CloudFormatAttrBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_format_exports_are_unique(self):
        self.assertIn("format_duration_secs", self.mod.CLOUD_FORMAT_EXPORTS)
        self.assertIn("summarize_runner_selector", self.mod.CLOUD_FORMAT_EXPORTS)
        self.assertEqual(len(self.mod.CLOUD_FORMAT_EXPORTS), len(set(self.mod.CLOUD_FORMAT_EXPORTS)))

    def test_install_cloud_format_attr_helpers_wires_late_bound_exports(self):
        calls = []

        def summarize_runner_selector(value):
            calls.append(("summarize_runner_selector", value))
            return "linux,arm64"

        bindings = {"_cloud": types.SimpleNamespace(summarize_runner_selector=summarize_runner_selector)}

        self.mod.install_cloud_format_attr_helpers(bindings, ("summarize_runner_selector",))

        self.assertEqual(bindings["summarize_runner_selector"]('["linux", "arm64"]'), "linux,arm64")
        self.assertEqual(calls, [("summarize_runner_selector", '["linux", "arm64"]')])


if __name__ == "__main__":
    unittest.main()
