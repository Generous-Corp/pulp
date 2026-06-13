#!/usr/bin/env python3
"""Tests for GitHub workflow settings facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("github_workflow_settings_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class GithubWorkflowSettingsBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_settings_exports_match_wrappers(self):
        expected = (
            "github_actions_settings_for_display",
            "resolve_github_actions_settings",
            "normalize_runs_on_json",
        )

        self.assertEqual(self.mod.GITHUB_WORKFLOW_SETTINGS_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_settings_bindings_delegate_to_github_workflows_module(self):
        calls = []

        def record(name, value):
            def wrapper(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return wrapper

        workflows = types.SimpleNamespace(
            github_actions_settings_for_display=record("github_actions_settings_for_display", {"workflow": "build"}),
            resolve_github_actions_settings=record("resolve_github_actions_settings", {"provider": "namespace"}),
            normalize_runs_on_json=record("normalize_runs_on_json", '"macos-15"'),
        )
        bindings = {"_github_workflows": workflows}
        config = {"github_actions": {}}

        self.assertEqual(self.mod.github_actions_settings_for_display(bindings, config), {"workflow": "build"})
        self.assertEqual(self.mod.resolve_github_actions_settings(bindings, config), {"provider": "namespace"})
        self.assertEqual(self.mod.normalize_runs_on_json(bindings, "macos-15", setting_name="setting"), '"macos-15"')

        self.assertEqual(
            [call[0] for call in calls],
            ["github_actions_settings_for_display", "resolve_github_actions_settings", "normalize_runs_on_json"],
        )
        self.assertEqual(calls[2][1], ("macos-15",))
        self.assertEqual(calls[2][2], {"setting_name": "setting"})

    def test_install_github_workflow_settings_helpers_wires_named_exports(self):
        calls = []

        def resolve_settings(config):
            calls.append(("resolve_github_actions_settings", config))
            return {"provider": "github-hosted"}

        workflows = types.SimpleNamespace(resolve_github_actions_settings=resolve_settings)
        bindings = {"_github_workflows": workflows}

        self.mod.install_github_workflow_settings_helpers(bindings, ("resolve_github_actions_settings",))

        self.assertEqual(bindings["resolve_github_actions_settings"]({"github_actions": {}}), {"provider": "github-hosted"})
        self.assertEqual(calls, [("resolve_github_actions_settings", {"github_actions": {}})])

    def test_install_github_workflow_settings_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_github_workflow_settings_helper = lambda _bindings: "future"

        self.mod.install_github_workflow_settings_helpers(bindings, ("future_github_workflow_settings_helper",))

        self.assertEqual(bindings["future_github_workflow_settings_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
