#!/usr/bin/env python3
"""Tests for GitHub workflow resolution facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("github_workflow_resolution_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class GithubWorkflowResolutionBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_resolution_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.GITHUB_WORKFLOW_SETTINGS_EXPORTS,
            *self.mod.GITHUB_WORKFLOW_DISPATCH_EXPORTS,
            *self.mod.GITHUB_WORKFLOW_PROVIDER_EXPORTS,
        )

        self.assertEqual(self.mod.GITHUB_WORKFLOW_RESOLUTION_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_resolution_helpers_routes_each_group(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_github_workflow_settings_helpers") as install_settings,
            mock.patch.object(self.mod, "install_github_workflow_dispatch_helpers") as install_dispatch,
            mock.patch.object(self.mod, "install_github_workflow_provider_helpers") as install_provider,
        ):
            self.mod.install_github_workflow_resolution_helpers(
                bindings,
                (
                    "resolve_github_actions_settings",
                    "resolve_cli_dispatch_field_values",
                    "resolve_default_provider_for_workflow",
                ),
            )

        install_settings.assert_called_once_with(bindings, ("resolve_github_actions_settings",))
        install_dispatch.assert_called_once_with(bindings, ("resolve_cli_dispatch_field_values",))
        install_provider.assert_called_once_with(bindings, ("resolve_default_provider_for_workflow",))

    def test_install_resolution_helpers_preserves_unknown_fallbacks(self):
        bindings = {"custom": object()}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_github_workflow_resolution_helpers(bindings, ("custom",))

        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
