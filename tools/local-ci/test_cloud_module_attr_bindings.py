#!/usr/bin/env python3
"""Tests for direct cloud module-attribute binding installer groups."""

from __future__ import annotations

from pathlib import Path
import unittest
from unittest import mock

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

    def test_install_cloud_module_attr_helpers_routes_each_group(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_cloud_billing_attr_helpers") as install_billing,
            mock.patch.object(self.mod, "install_cloud_record_store_attr_helpers") as install_record_store,
            mock.patch.object(self.mod, "install_cloud_github_attr_helpers") as install_github,
            mock.patch.object(self.mod, "install_cloud_namespace_attr_helpers") as install_namespace,
            mock.patch.object(self.mod, "install_cloud_format_attr_helpers") as install_format,
            mock.patch.object(self.mod, "install_module_attrs") as install_module_attrs,
        ):
            self.mod.install_cloud_module_attr_helpers(
                bindings,
                (
                    "estimate_cloud_record_cost",
                    "normalize_cloud_record",
                    "gh_repo_variables",
                    "nsc_available",
                    "summarize_runner_selector",
                    "replacement_only",
                ),
            )

        install_billing.assert_called_once_with(bindings, ("estimate_cloud_record_cost",))
        install_record_store.assert_called_once_with(bindings, ("normalize_cloud_record",))
        install_github.assert_called_once_with(bindings, ("gh_repo_variables",))
        install_namespace.assert_called_once_with(bindings, ("nsc_available",))
        install_format.assert_called_once_with(bindings, ("summarize_runner_selector",))
        install_module_attrs.assert_called_once_with(bindings, "_cloud", ("replacement_only",))


if __name__ == "__main__":
    unittest.main()
