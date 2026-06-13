#!/usr/bin/env python3
"""Tests for GitHub workflow dispatch-field facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("github_workflow_dispatch_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class GithubWorkflowDispatchBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_dispatch_bindings_delegate_to_github_workflows_module(self):
        calls = []

        def record(name, value):
            def wrapper(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return wrapper

        workflows = types.SimpleNamespace(
            resolve_workflow_runner_selector_json=record("resolve_workflow_runner_selector_json", '["self-hosted"]'),
            resolve_workflow_dispatch_field_values=record("resolve_workflow_dispatch_field_values", {"field": "value"}),
            repo_variable_name_for_workflow_field=record("repo_variable_name_for_workflow_field", "PULP_VAR"),
            resolve_workflow_field_value_and_source=record("resolve_workflow_field_value_and_source", ("value", "source")),
            resolve_workflow_dispatch_defaults=record(
                "resolve_workflow_dispatch_defaults",
                ({"field": "value"}, {"field": "source"}),
            ),
            resolve_cli_dispatch_field_values=record("resolve_cli_dispatch_field_values", {"field": "cli"}),
        )
        bindings = {"_github_workflows": workflows}
        config = {"github_actions": {}}
        repository_variables = {"PULP_VAR": '"ubuntu-latest"'}
        fields = ("runner_selector_json",)

        self.assertEqual(
            self.mod.resolve_workflow_runner_selector_json(bindings, config, "docs-check", "namespace"),
            '["self-hosted"]',
        )
        self.assertEqual(
            self.mod.resolve_workflow_dispatch_field_values(bindings, config, "build", "namespace", fields),
            {"field": "value"},
        )
        self.assertEqual(
            self.mod.repo_variable_name_for_workflow_field(bindings, "build", "namespace", "linux_runner_selector_json"),
            "PULP_VAR",
        )
        self.assertEqual(
            self.mod.resolve_workflow_field_value_and_source(
                bindings,
                config,
                repository_variables,
                "build",
                "namespace",
                "linux_runner_selector_json",
            ),
            ("value", "source"),
        )
        self.assertEqual(
            self.mod.resolve_workflow_dispatch_defaults(bindings, config, repository_variables, "build", "namespace", fields),
            ({"field": "value"}, {"field": "source"}),
        )
        args = types.SimpleNamespace(linux_runner_selector_json=None)
        self.assertEqual(self.mod.resolve_cli_dispatch_field_values(bindings, args, fields), {"field": "cli"})

        self.assertEqual(
            [call[0] for call in calls],
            [
                "resolve_workflow_runner_selector_json",
                "resolve_workflow_dispatch_field_values",
                "repo_variable_name_for_workflow_field",
                "resolve_workflow_field_value_and_source",
                "resolve_workflow_dispatch_defaults",
                "resolve_cli_dispatch_field_values",
            ],
        )
        self.assertEqual(calls[3][1], (config, repository_variables, "build", "namespace", "linux_runner_selector_json"))
        self.assertEqual(calls[5][1], (args, fields))


if __name__ == "__main__":
    unittest.main()
