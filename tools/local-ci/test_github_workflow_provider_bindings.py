#!/usr/bin/env python3
"""Tests for GitHub workflow provider facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("github_workflow_provider_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class GithubWorkflowProviderBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_provider_exports_match_wrappers(self):
        expected = (
            "resolve_default_provider_for_workflow",
            "summarize_workflow_provider_defaults",
        )

        self.assertEqual(self.mod.GITHUB_WORKFLOW_PROVIDER_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_provider_bindings_delegate_to_github_workflows_module(self):
        calls = []

        def record(name, value):
            def wrapper(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return wrapper

        workflows = types.SimpleNamespace(
            resolve_default_provider_for_workflow=record("resolve_default_provider_for_workflow", ("github-hosted", "builtin")),
            summarize_workflow_provider_defaults=record("summarize_workflow_provider_defaults", {"provider": "github-hosted"}),
        )
        bindings = {"_github_workflows": workflows}
        config = {"github_actions": {}}
        repository_variables = {"PULP_VAR": '"ubuntu-latest"'}

        self.assertEqual(
            self.mod.resolve_default_provider_for_workflow(bindings, {"provider": "namespace"}, "build", explicit_provider="github-hosted"),
            ("github-hosted", "builtin"),
        )
        self.assertEqual(
            self.mod.summarize_workflow_provider_defaults(bindings, config, repository_variables, {"provider": "namespace"}, "build"),
            {"provider": "github-hosted"},
        )

        self.assertEqual(
            [call[0] for call in calls],
            ["resolve_default_provider_for_workflow", "summarize_workflow_provider_defaults"],
        )
        self.assertEqual(calls[0][2], {"explicit_provider": "github-hosted"})
        self.assertEqual(calls[1][1], (config, repository_variables, {"provider": "namespace"}, "build"))

    def test_install_github_workflow_provider_helpers_wires_named_exports(self):
        calls = []

        def resolve_provider(settings, workflow_key, *, explicit_provider=None):
            calls.append(("resolve_default_provider_for_workflow", settings, workflow_key, explicit_provider))
            return "github-hosted", "builtin"

        workflows = types.SimpleNamespace(resolve_default_provider_for_workflow=resolve_provider)
        bindings = {"_github_workflows": workflows}

        self.mod.install_github_workflow_provider_helpers(bindings, ("resolve_default_provider_for_workflow",))

        self.assertEqual(
            bindings["resolve_default_provider_for_workflow"]({"provider": "namespace"}, "build", explicit_provider="github-hosted"),
            ("github-hosted", "builtin"),
        )
        self.assertEqual(
            calls,
            [("resolve_default_provider_for_workflow", {"provider": "namespace"}, "build", "github-hosted")],
        )


if __name__ == "__main__":
    unittest.main()
