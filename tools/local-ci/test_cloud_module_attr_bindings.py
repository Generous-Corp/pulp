#!/usr/bin/env python3
"""Tests for direct cloud module-attribute binding installer groups."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("cloud_module_attr_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudModuleAttrBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_module_attr_exports_are_grouped_without_overlap(self):
        groups = (
            self.mod.CLOUD_BILLING_EXPORTS,
            self.mod.CLOUD_RECORD_STORE_EXPORTS,
            self.mod.CLOUD_GITHUB_MODULE_EXPORTS,
            self.mod.CLOUD_NAMESPACE_EXPORTS,
            self.mod.CLOUD_FORMAT_EXPORTS,
        )
        flattened = tuple(name for group in groups for name in group)

        self.assertEqual(self.mod.CLOUD_MODULE_ATTR_EXPORTS, flattened)
        self.assertEqual(len(flattened), len(set(flattened)))

    def test_install_cloud_module_attr_helpers_wires_late_bound_exports(self):
        calls = []

        def summarize_runner_selector(value):
            calls.append(("summarize_runner_selector", value))
            return "linux,arm64"

        cloud = types.SimpleNamespace(summarize_runner_selector=summarize_runner_selector)
        bindings = {"_cloud": cloud}

        self.mod.install_cloud_module_attr_helpers(bindings, ("summarize_runner_selector",))

        self.assertEqual(bindings["summarize_runner_selector"]('["linux", "arm64"]'), "linux,arm64")
        self.assertEqual(calls, [("summarize_runner_selector", '["linux", "arm64"]')])

        def replacement(value):
            calls.append(("replacement", value))
            return "replacement"

        bindings["_cloud"].summarize_runner_selector = replacement
        self.assertEqual(bindings["summarize_runner_selector"]('"macos-15"'), "replacement")
        self.assertEqual(calls[-1], ("replacement", '"macos-15"'))


if __name__ == "__main__":
    unittest.main()
