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

    def test_install_cloud_module_attr_helpers_routes_each_group(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        cloud = types.SimpleNamespace(
            estimate_cloud_record_cost=make_runner("estimate_cloud_record_cost", 1.0),
            normalize_cloud_record=make_runner("normalize_cloud_record", {"normalized": True}),
            gh_repo_variables=make_runner("gh_repo_variables", {"PULP_VAR": "value"}),
            nsc_available=make_runner("nsc_available", True),
            summarize_runner_selector=make_runner("summarize_runner_selector", "linux,arm64"),
        )
        bindings = {"_cloud": cloud}

        self.mod.install_cloud_module_attr_helpers(
            bindings,
            (
                "estimate_cloud_record_cost",
                "normalize_cloud_record",
                "gh_repo_variables",
                "nsc_available",
                "summarize_runner_selector",
            ),
        )

        self.assertEqual(bindings["estimate_cloud_record_cost"]({"provider": "github"}, {}), 1.0)
        self.assertEqual(bindings["normalize_cloud_record"]({"id": "run"}), {"normalized": True})
        self.assertEqual(bindings["gh_repo_variables"]("owner/repo"), {"PULP_VAR": "value"})
        self.assertTrue(bindings["nsc_available"]())
        self.assertEqual(bindings["summarize_runner_selector"]("selector"), "linux,arm64")
        self.assertEqual(
            [call[0] for call in calls],
            [
                "estimate_cloud_record_cost",
                "normalize_cloud_record",
                "gh_repo_variables",
                "nsc_available",
                "summarize_runner_selector",
            ],
        )

    def test_install_cloud_module_attr_helpers_keeps_unknown_module_attr_support(self):
        def replacement_only(value):
            return f"replacement:{value}"

        bindings = {"_cloud": types.SimpleNamespace(replacement_only=replacement_only)}

        self.mod.install_cloud_module_attr_helpers(bindings, ("replacement_only",))

        self.assertEqual(bindings["replacement_only"]("value"), "replacement:value")


if __name__ == "__main__":
    unittest.main()
