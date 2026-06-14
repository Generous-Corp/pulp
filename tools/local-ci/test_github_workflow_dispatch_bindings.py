#!/usr/bin/env python3
"""Tests for GitHub workflow dispatch-field facade bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("github_workflow_dispatch_bindings.py")


class GithubWorkflowDispatchBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_dispatch_exports_match_wrappers(self):
        expected = (
            *self.mod.GITHUB_WORKFLOW_DISPATCH_SELECTOR_EXPORTS,
            *self.mod.GITHUB_WORKFLOW_DISPATCH_FIELD_EXPORTS,
            *self.mod.GITHUB_WORKFLOW_DISPATCH_DEFAULT_EXPORTS,
            *self.mod.GITHUB_WORKFLOW_DISPATCH_CLI_EXPORTS,
        )

        self.assertEqual(self.mod.GITHUB_WORKFLOW_DISPATCH_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_github_workflow_dispatch_helpers_routes_each_group_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_github_workflow_dispatch_selector_helpers") as install_selector,
            mock.patch.object(self.mod, "install_github_workflow_dispatch_field_helpers") as install_field,
            mock.patch.object(self.mod, "install_github_workflow_dispatch_default_helpers") as install_default,
            mock.patch.object(self.mod, "install_github_workflow_dispatch_cli_helpers") as install_cli,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_github_workflow_dispatch_helpers(
                bindings,
                (
                    "resolve_workflow_runner_selector_json",
                    "repo_variable_name_for_workflow_field",
                    "resolve_workflow_dispatch_defaults",
                    "resolve_cli_dispatch_field_values",
                    "custom",
                ),
            )

        install_selector.assert_called_once_with(bindings, ("resolve_workflow_runner_selector_json",))
        install_field.assert_called_once_with(bindings, ("repo_variable_name_for_workflow_field",))
        install_default.assert_called_once_with(bindings, ("resolve_workflow_dispatch_defaults",))
        install_cli.assert_called_once_with(bindings, ("resolve_cli_dispatch_field_values",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
