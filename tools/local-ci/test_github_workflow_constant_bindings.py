#!/usr/bin/env python3
"""Tests for GitHub workflow constant facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("github_workflow_constant_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class GithubWorkflowConstantBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_are_named_constant_helpers(self):
        expected = (
            "github_actions_defaults",
            "builtin_github_workflows",
            "repo_variable_fallbacks",
        )

        self.assertEqual(self.mod.GITHUB_WORKFLOW_CONSTANT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_constants_delegate_to_github_workflows_module(self):
        workflows = types.SimpleNamespace(
            GITHUB_ACTIONS_DEFAULTS={"provider": "github-hosted"},
            BUILTIN_GITHUB_WORKFLOWS={"build": {"workflow_file": "build.yml"}},
            REPO_VARIABLE_FALLBACKS={"PULP_VAR": "fallback"},
        )
        bindings = {"_github_workflows": workflows}

        self.assertEqual(self.mod.github_actions_defaults(bindings), {"provider": "github-hosted"})
        self.assertEqual(self.mod.builtin_github_workflows(bindings), {"build": {"workflow_file": "build.yml"}})
        self.assertEqual(self.mod.repo_variable_fallbacks(bindings), {"PULP_VAR": "fallback"})

    def test_install_github_workflow_constant_helpers_wires_named_exports(self):
        workflows = types.SimpleNamespace(
            GITHUB_ACTIONS_DEFAULTS={"provider": "github-hosted"},
            BUILTIN_GITHUB_WORKFLOWS={"build": {"workflow_file": "build.yml"}},
            REPO_VARIABLE_FALLBACKS={"PULP_VAR": "fallback"},
        )
        bindings = {"_github_workflows": workflows}

        self.mod.install_github_workflow_constant_helpers(
            bindings,
            ("github_actions_defaults", "repo_variable_fallbacks"),
        )

        self.assertEqual(bindings["github_actions_defaults"](), {"provider": "github-hosted"})
        self.assertEqual(bindings["repo_variable_fallbacks"](), {"PULP_VAR": "fallback"})


if __name__ == "__main__":
    unittest.main()
