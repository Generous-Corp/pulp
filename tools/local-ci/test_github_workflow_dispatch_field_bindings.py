#!/usr/bin/env python3
"""Tests for GitHub workflow dispatch field bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("github_workflow_dispatch_field_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class GithubWorkflowDispatchFieldBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_field_exports_match_wrappers(self):
        expected = (
            "resolve_workflow_dispatch_field_values",
            "repo_variable_name_for_workflow_field",
            "resolve_workflow_field_value_and_source",
        )

        self.assertEqual(self.mod.GITHUB_WORKFLOW_DISPATCH_FIELD_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_field_bindings_delegate_to_github_workflows_module(self):
        calls = []

        def field_values(config, workflow_key, provider, field_names):
            calls.append(("field_values", (config, workflow_key, provider, field_names)))
            return {"runner_selector_json": '"macos-15"'}

        def variable_name(workflow_key, provider, field_name):
            calls.append(("variable_name", (workflow_key, provider, field_name)))
            return "PULP_LOCAL_MACOS_RUNS_ON_JSON"

        def value_source(config, repository_variables, workflow_key, provider, field_name):
            calls.append(("value_source", (config, repository_variables, workflow_key, provider, field_name)))
            return '"macos-15"', "repo-var"

        workflows = types.SimpleNamespace(
            resolve_workflow_dispatch_field_values=field_values,
            repo_variable_name_for_workflow_field=variable_name,
            resolve_workflow_field_value_and_source=value_source,
        )
        bindings = {"_github_workflows": workflows}
        config = {"github_actions": {}}
        repository_variables = {"PULP_LOCAL_MACOS_RUNS_ON_JSON": '"macos-15"'}

        self.assertEqual(
            self.mod.resolve_workflow_dispatch_field_values(bindings, config, "build", "github-hosted", ("runner_selector_json",)),
            {"runner_selector_json": '"macos-15"'},
        )
        self.assertEqual(
            self.mod.repo_variable_name_for_workflow_field(bindings, "build", "github-hosted", "macos_runner_selector_json"),
            "PULP_LOCAL_MACOS_RUNS_ON_JSON",
        )
        self.assertEqual(
            self.mod.resolve_workflow_field_value_and_source(
                bindings,
                config,
                repository_variables,
                "build",
                "github-hosted",
                "macos_runner_selector_json",
            ),
            ('"macos-15"', "repo-var"),
        )
        self.assertEqual(
            calls,
            [
                ("field_values", (config, "build", "github-hosted", ("runner_selector_json",))),
                ("variable_name", ("build", "github-hosted", "macos_runner_selector_json")),
                ("value_source", (config, repository_variables, "build", "github-hosted", "macos_runner_selector_json")),
            ],
        )


if __name__ == "__main__":
    unittest.main()
