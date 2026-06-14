#!/usr/bin/env python3
"""Tests for GitHub workflow dispatch selector bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("github_workflow_dispatch_selector_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class GithubWorkflowDispatchSelectorBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_selector_exports_match_wrappers(self):
        expected = ("resolve_workflow_runner_selector_json",)

        self.assertEqual(self.mod.GITHUB_WORKFLOW_DISPATCH_SELECTOR_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_selector_binding_delegates_to_github_workflows_module(self):
        calls = []

        def resolve(config, workflow_key, provider):
            calls.append((config, workflow_key, provider))
            return '["self-hosted"]'

        workflows = types.SimpleNamespace(resolve_workflow_runner_selector_json=resolve)
        bindings = {"_github_workflows": workflows}
        config = {"github_actions": {}}

        self.assertEqual(
            self.mod.resolve_workflow_runner_selector_json(bindings, config, "docs-check", "namespace"),
            '["self-hosted"]',
        )
        self.assertEqual(calls, [(config, "docs-check", "namespace")])


if __name__ == "__main__":
    unittest.main()
