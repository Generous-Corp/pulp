#!/usr/bin/env python3
"""Tests for GitHub workflow resolver facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("github_workflow_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class GithubWorkflowBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_github_workflow_exports_are_composed_from_focused_groups(self):
        expected = self.mod.GITHUB_WORKFLOW_RESOLUTION_EXPORTS

        self.assertEqual(self.mod.GITHUB_WORKFLOW_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        self.assertEqual(
            self.mod.GITHUB_WORKFLOW_CONSTANT_EXPORTS,
            (
                "github_actions_defaults",
                "builtin_github_workflows",
                "repo_variable_fallbacks",
            ),
        )

    def test_install_github_workflow_helpers_routes_resolution_and_constant_groups(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_github_workflow_constant_helpers") as constant,
            mock.patch.object(self.mod, "install_github_workflow_resolution_helpers") as resolution,
        ):
            self.mod.install_github_workflow_helpers(
                bindings,
                (
                    "resolve_github_actions_settings",
                    "resolve_cli_dispatch_field_values",
                    "resolve_default_provider_for_workflow",
                    "github_actions_defaults",
                ),
            )

        constant.assert_called_once_with(bindings, ("github_actions_defaults",))
        resolution.assert_called_once_with(
            bindings,
            (
                "resolve_github_actions_settings",
                "resolve_cli_dispatch_field_values",
                "resolve_default_provider_for_workflow",
            ),
        )


if __name__ == "__main__":
    unittest.main()
