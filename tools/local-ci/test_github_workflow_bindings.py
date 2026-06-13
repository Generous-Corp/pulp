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

    def _bindings(self):
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
        return {"_github_workflows": workflows}, calls

    def test_constants_delegate_to_github_workflows_module(self):
        bindings, _calls = self._bindings()

        self.assertEqual(self.mod.github_actions_defaults(bindings), {"provider": "github-hosted"})
        self.assertEqual(self.mod.builtin_github_workflows(bindings), {"build": {"workflow_file": "build.yml"}})
        self.assertEqual(self.mod.repo_variable_fallbacks(bindings), {"PULP_VAR": "fallback"})

    def test_settings_helpers_delegate_to_github_workflows_module(self):
        bindings, calls = self._bindings()
        config = {"github_actions": {}}

        self.assertEqual(self.mod.github_actions_settings_for_display(bindings, config), {"workflow": "build"})
        self.assertEqual(self.mod.resolve_github_actions_settings(bindings, config), {"provider": "namespace"})
        self.assertEqual(self.mod.normalize_runs_on_json(bindings, "macos-15", setting_name="setting"), '"macos-15"')

        self.assertEqual([call[0] for call in calls], [
            "github_actions_settings_for_display",
            "resolve_github_actions_settings",
            "normalize_runs_on_json",
        ])
        self.assertEqual(calls[2][1], ("macos-15",))
        self.assertEqual(calls[2][2], {"setting_name": "setting"})

    def test_workflow_resolution_helpers_delegate_to_github_workflows_module(self):
        bindings, calls = self._bindings()
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
            self.mod.resolve_default_provider_for_workflow(bindings, {"provider": "namespace"}, "build", explicit_provider="github-hosted"),
            ("github-hosted", "builtin"),
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
        self.assertEqual(
            self.mod.summarize_workflow_provider_defaults(bindings, config, repository_variables, {"provider": "namespace"}, "build"),
            {"provider": "github-hosted"},
        )
        args = types.SimpleNamespace(linux_runner_selector_json=None)
        self.assertEqual(self.mod.resolve_cli_dispatch_field_values(bindings, args, fields), {"field": "cli"})

        self.assertEqual([call[0] for call in calls], [
            "resolve_workflow_runner_selector_json",
            "resolve_workflow_dispatch_field_values",
            "repo_variable_name_for_workflow_field",
            "resolve_default_provider_for_workflow",
            "resolve_workflow_field_value_and_source",
            "resolve_workflow_dispatch_defaults",
            "summarize_workflow_provider_defaults",
            "resolve_cli_dispatch_field_values",
        ])
        self.assertEqual(calls[3][2], {"explicit_provider": "github-hosted"})
        self.assertEqual(calls[4][1], (config, repository_variables, "build", "namespace", "linux_runner_selector_json"))
        self.assertEqual(calls[7][1], (args, fields))

    def test_install_github_workflow_helpers_wires_named_exports(self):
        bindings, calls = self._bindings()

        self.mod.install_github_workflow_helpers(
            bindings,
            ("resolve_github_actions_settings", "normalize_runs_on_json"),
        )

        self.assertEqual(bindings["resolve_github_actions_settings"]({"github_actions": {}}), {"provider": "namespace"})
        self.assertEqual(bindings["normalize_runs_on_json"]("macos-15", setting_name="runner"), '"macos-15"')
        self.assertEqual([call[0] for call in calls], [
            "resolve_github_actions_settings",
            "normalize_runs_on_json",
        ])
        self.assertEqual(calls[1][2], {"setting_name": "runner"})

    def test_install_github_workflow_helpers_routes_each_group(self):
        bindings, calls = self._bindings()

        self.mod.install_github_workflow_helpers(
            bindings,
            (
                "resolve_github_actions_settings",
                "resolve_cli_dispatch_field_values",
                "resolve_default_provider_for_workflow",
            ),
        )

        args = types.SimpleNamespace(linux_runner_selector_json=None)
        self.assertEqual(bindings["resolve_github_actions_settings"]({"github_actions": {}}), {"provider": "namespace"})
        self.assertEqual(bindings["resolve_cli_dispatch_field_values"](args, ("field",)), {"field": "cli"})
        self.assertEqual(
            bindings["resolve_default_provider_for_workflow"]({"provider": "namespace"}, "build"),
            ("github-hosted", "builtin"),
        )
        self.assertEqual(
            [call[0] for call in calls],
            [
                "resolve_github_actions_settings",
                "resolve_cli_dispatch_field_values",
                "resolve_default_provider_for_workflow",
            ],
        )

    def test_install_github_workflow_helpers_keeps_unknown_imported_name_support(self):
        bindings, _calls = self._bindings()

        self.mod.install_github_workflow_helpers(bindings, ("github_actions_defaults",))

        self.assertEqual(bindings["github_actions_defaults"](), {"provider": "github-hosted"})


if __name__ == "__main__":
    unittest.main()
