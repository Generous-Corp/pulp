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

    def test_install_github_workflow_dispatch_helpers_wires_named_exports(self):
        calls = []

        def resolve_cli(args, field_names):
            calls.append(("resolve_cli_dispatch_field_values", args, field_names))
            return {"field": "cli"}

        workflows = types.SimpleNamespace(resolve_cli_dispatch_field_values=resolve_cli)
        bindings = {"_github_workflows": workflows}

        self.mod.install_github_workflow_dispatch_helpers(bindings, ("resolve_cli_dispatch_field_values",))

        args = types.SimpleNamespace(linux_runner_selector_json=None)
        self.assertEqual(bindings["resolve_cli_dispatch_field_values"](args, ("field",)), {"field": "cli"})
        self.assertEqual(calls, [("resolve_cli_dispatch_field_values", args, ("field",))])

    def test_install_github_workflow_dispatch_helpers_routes_each_group(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        workflows = types.SimpleNamespace(
            resolve_workflow_runner_selector_json=make_runner("resolve_workflow_runner_selector_json", '["self-hosted"]'),
            repo_variable_name_for_workflow_field=make_runner("repo_variable_name_for_workflow_field", "PULP_VAR"),
            resolve_workflow_dispatch_defaults=make_runner(
                "resolve_workflow_dispatch_defaults",
                ({"field": "value"}, {"field": "source"}),
            ),
            resolve_cli_dispatch_field_values=make_runner("resolve_cli_dispatch_field_values", {"field": "cli"}),
        )
        bindings = {"_github_workflows": workflows}

        self.mod.install_github_workflow_dispatch_helpers(
            bindings,
            (
                "resolve_workflow_runner_selector_json",
                "repo_variable_name_for_workflow_field",
                "resolve_workflow_dispatch_defaults",
                "resolve_cli_dispatch_field_values",
            ),
        )

        args = types.SimpleNamespace(linux_runner_selector_json=None)
        self.assertEqual(bindings["resolve_workflow_runner_selector_json"]({}, "build", "namespace"), '["self-hosted"]')
        self.assertEqual(bindings["repo_variable_name_for_workflow_field"]("build", "namespace", "linux_runner_selector_json"), "PULP_VAR")
        self.assertEqual(
            bindings["resolve_workflow_dispatch_defaults"]({}, {}, "build", "namespace", ("field",)),
            ({"field": "value"}, {"field": "source"}),
        )
        self.assertEqual(bindings["resolve_cli_dispatch_field_values"](args, ("field",)), {"field": "cli"})
        self.assertEqual(
            [call[0] for call in calls],
            [
                "resolve_workflow_runner_selector_json",
                "repo_variable_name_for_workflow_field",
                "resolve_workflow_dispatch_defaults",
                "resolve_cli_dispatch_field_values",
            ],
        )

    def test_install_github_workflow_dispatch_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_github_workflow_dispatch_helper = lambda _bindings: "future"

        self.mod.install_github_workflow_dispatch_helpers(bindings, ("future_github_workflow_dispatch_helper",))

        self.assertEqual(bindings["future_github_workflow_dispatch_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
