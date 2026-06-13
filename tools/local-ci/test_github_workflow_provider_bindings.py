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


if __name__ == "__main__":
    unittest.main()
