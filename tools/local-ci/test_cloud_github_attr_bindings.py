#!/usr/bin/env python3
"""Tests for direct cloud GitHub module-attribute binding installer."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("cloud_github_attr_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudGithubAttrBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_github_exports_are_unique(self):
        self.assertIn("gh_repo_variables", self.mod.CLOUD_GITHUB_MODULE_EXPORTS)
        self.assertIn("resolve_github_repository", self.mod.CLOUD_GITHUB_MODULE_EXPORTS)
        self.assertEqual(len(self.mod.CLOUD_GITHUB_MODULE_EXPORTS), len(set(self.mod.CLOUD_GITHUB_MODULE_EXPORTS)))

    def test_install_cloud_github_attr_helpers_wires_late_bound_exports(self):
        calls = []

        def gh_repo_variables(repo):
            calls.append(("gh_repo_variables", repo))
            return {"PULP_VAR": "value"}

        bindings = {"_cloud": types.SimpleNamespace(gh_repo_variables=gh_repo_variables)}

        self.mod.install_cloud_github_attr_helpers(bindings, ("gh_repo_variables",))

        self.assertEqual(bindings["gh_repo_variables"]("owner/repo"), {"PULP_VAR": "value"})
        self.assertEqual(calls, [("gh_repo_variables", "owner/repo")])


if __name__ == "__main__":
    unittest.main()
