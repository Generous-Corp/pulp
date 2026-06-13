#!/usr/bin/env python3
"""Tests for GitHub workflow dispatch default bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("github_workflow_dispatch_default_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class GithubWorkflowDispatchDefaultBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_default_exports_match_wrappers(self):
        expected = ("resolve_workflow_dispatch_defaults",)

        self.assertEqual(self.mod.GITHUB_WORKFLOW_DISPATCH_DEFAULT_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_default_binding_delegates_to_github_workflows_module(self):
        calls = []

        def defaults(config, repository_variables, workflow_key, provider, field_names):
            calls.append((config, repository_variables, workflow_key, provider, field_names))
            return {"field": "value"}, {"field": "source"}

        workflows = types.SimpleNamespace(resolve_workflow_dispatch_defaults=defaults)
        bindings = {"_github_workflows": workflows}
        config = {"github_actions": {}}
        repository_variables = {"PULP_VAR": "value"}

        self.assertEqual(
            self.mod.resolve_workflow_dispatch_defaults(bindings, config, repository_variables, "build", "namespace", ("field",)),
            ({"field": "value"}, {"field": "source"}),
        )
        self.assertEqual(calls, [(config, repository_variables, "build", "namespace", ("field",))])

    def test_install_default_helpers_wires_named_exports(self):
        calls = []

        def defaults(config, repository_variables, workflow_key, provider, field_names):
            calls.append((config, repository_variables, workflow_key, provider, field_names))
            return {"field": "value"}, {"field": "source"}

        workflows = types.SimpleNamespace(resolve_workflow_dispatch_defaults=defaults)
        bindings = {"_github_workflows": workflows}

        self.mod.install_github_workflow_dispatch_default_helpers(bindings)

        self.assertEqual(
            bindings["resolve_workflow_dispatch_defaults"]({}, {}, "build", "namespace", ("field",)),
            ({"field": "value"}, {"field": "source"}),
        )
        self.assertEqual(calls, [({}, {}, "build", "namespace", ("field",))])

    def test_install_default_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_github_workflow_dispatch_default_helper = lambda _bindings: "future"

        self.mod.install_github_workflow_dispatch_default_helpers(bindings, ("future_github_workflow_dispatch_default_helper",))

        self.assertEqual(bindings["future_github_workflow_dispatch_default_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
