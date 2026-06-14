#!/usr/bin/env python3
"""Tests for GitHub workflow CLI dispatch bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("github_workflow_dispatch_cli_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class GithubWorkflowDispatchCliBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_cli_exports_match_wrappers(self):
        expected = ("resolve_cli_dispatch_field_values",)

        self.assertEqual(self.mod.GITHUB_WORKFLOW_DISPATCH_CLI_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_cli_binding_delegates_to_github_workflows_module(self):
        calls = []

        def cli_values(args, field_names):
            calls.append((args, field_names))
            return {"field": "cli"}

        workflows = types.SimpleNamespace(resolve_cli_dispatch_field_values=cli_values)
        bindings = {"_github_workflows": workflows}
        args = types.SimpleNamespace(linux_runner_selector_json=None)

        self.assertEqual(self.mod.resolve_cli_dispatch_field_values(bindings, args, ("field",)), {"field": "cli"})
        self.assertEqual(calls, [(args, ("field",))])


if __name__ == "__main__":
    unittest.main()
