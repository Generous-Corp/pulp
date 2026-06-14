#!/usr/bin/env python3
"""Tests for GitHub workflow resolver facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
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
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        workflows = types.SimpleNamespace(
            GITHUB_ACTIONS_DEFAULTS={"provider": "github-hosted"},
            BUILTIN_GITHUB_WORKFLOWS={"build": {"workflow_file": "build.yml"}},
            REPO_VARIABLE_FALLBACKS={"PULP_VAR": "fallback"},
            github_actions_settings_for_display=make_runner("github_actions_settings_for_display", {"workflow": "build"}),
            resolve_github_actions_settings=make_runner("resolve_github_actions_settings", {"provider": "namespace"}),
            normalize_runs_on_json=make_runner("normalize_runs_on_json", '"macos-15"'),
            resolve_workflow_runner_selector_json=make_runner("resolve_workflow_runner_selector_json", '["self-hosted"]'),
            resolve_workflow_dispatch_field_values=make_runner("resolve_workflow_dispatch_field_values", {"field": "value"}),
            repo_variable_name_for_workflow_field=make_runner("repo_variable_name_for_workflow_field", "PULP_VAR"),
            resolve_default_provider_for_workflow=make_runner("resolve_default_provider_for_workflow", ("github-hosted", "builtin")),
            resolve_workflow_field_value_and_source=make_runner("resolve_workflow_field_value_and_source", ("value", "source")),
            resolve_workflow_dispatch_defaults=make_runner("resolve_workflow_dispatch_defaults", ({"field": "value"}, {"field": "source"})),
            summarize_workflow_provider_defaults=make_runner("summarize_workflow_provider_defaults", {"provider": "github-hosted"}),
            resolve_cli_dispatch_field_values=make_runner("resolve_cli_dispatch_field_values", {"field": "cli"}),
        )
        bindings = {"_github_workflows": workflows}

        self.mod.install_github_workflow_helpers(
            bindings,
            (
                "resolve_github_actions_settings",
                "resolve_cli_dispatch_field_values",
                "resolve_default_provider_for_workflow",
                "github_actions_defaults",
            ),
        )

        args = types.SimpleNamespace(linux_runner_selector_json=None)
        self.assertEqual(bindings["resolve_github_actions_settings"]({"github_actions": {}}), {"provider": "namespace"})
        self.assertEqual(bindings["resolve_cli_dispatch_field_values"](args, ("field",)), {"field": "cli"})
        self.assertEqual(
            bindings["resolve_default_provider_for_workflow"]({"provider": "namespace"}, "build"),
            ("github-hosted", "builtin"),
        )
        self.assertEqual(bindings["github_actions_defaults"](), {"provider": "github-hosted"})
        self.assertEqual(
            [call[0] for call in calls],
            [
                "resolve_github_actions_settings",
                "resolve_cli_dispatch_field_values",
                "resolve_default_provider_for_workflow",
            ],
        )


if __name__ == "__main__":
    unittest.main()
