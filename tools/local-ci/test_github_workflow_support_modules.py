#!/usr/bin/env python3
"""Tests for focused GitHub workflow support modules."""

from __future__ import annotations

import unittest
from types import SimpleNamespace

import github_workflow_config
import github_workflow_dispatch
import github_workflow_metadata
import github_workflow_provider
import github_workflow_settings


class GithubWorkflowSupportModuleTests(unittest.TestCase):
    def test_metadata_owns_builtin_workflows_and_repo_variable_fallbacks(self) -> None:
        self.assertEqual(
            github_workflow_metadata.BUILTIN_GITHUB_WORKFLOWS["build"]["file"],
            "build.yml",
        )
        self.assertEqual(
            github_workflow_metadata.repo_variable_name_for_workflow_field(
                "build",
                "namespace",
                "macos_runner_selector_json",
            ),
            "PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON",
        )
        self.assertEqual(
            github_workflow_metadata.repo_variable_name_for_workflow_field(
                "build",
                "github-hosted",
                "macos_runner_selector_json",
            ),
            "",
        )

    def test_settings_module_resolves_display_and_timing_defaults(self) -> None:
        config = {
            "github_actions": {
                "repository": "  danielraffel/pulp  ",
                "defaults": {
                    "workflow": "  docs-check  ",
                    "provider": "  namespace  ",
                    "wait_poll_secs": "7",
                    "match_timeout_secs": 42,
                },
            }
        }

        display = github_workflow_settings.github_actions_settings_for_display(config)
        self.assertEqual(display["repository"], "danielraffel/pulp")
        self.assertEqual(display["workflow"], "docs-check")
        self.assertEqual(display["provider"], "namespace")

        resolved = github_workflow_settings.resolve_github_actions_settings(config)
        self.assertEqual(resolved["wait_poll_secs"], 7)
        self.assertEqual(resolved["match_timeout_secs"], 42)

    def test_settings_module_normalizes_runs_on_json(self) -> None:
        self.assertEqual(
            github_workflow_settings.normalize_runs_on_json(
                '["self-hosted", "macOS"]',
                setting_name="selector",
            ),
            '["self-hosted", "macOS"]',
        )
        self.assertEqual(
            github_workflow_settings.normalize_runs_on_json(
                '"ubuntu-24.04"',
                setting_name="selector",
            ),
            '"ubuntu-24.04"',
        )
        with self.assertRaisesRegex(ValueError, "valid JSON"):
            github_workflow_settings.normalize_runs_on_json("macos-15", setting_name="selector")
        with self.assertRaisesRegex(ValueError, "string or array"):
            github_workflow_settings.normalize_runs_on_json("123", setting_name="selector")

    def test_config_module_returns_provider_info_only_for_valid_shapes(self) -> None:
        config = {
            "github_actions": {
                "workflows": {
                    "build": {
                        "providers": {
                            "namespace": {
                                "linux_runner_selector_json": '"namespace-linux"',
                            },
                        },
                    },
                },
            },
        }

        self.assertEqual(
            github_workflow_config.workflow_provider_config(config, "build", "namespace"),
            {"linux_runner_selector_json": '"namespace-linux"'},
        )
        malformed_configs = [
            {"github_actions": {"workflows": []}},
            {"github_actions": {"workflows": {"build": []}}},
            {"github_actions": {"workflows": {"build": {"providers": []}}}},
            {"github_actions": {"workflows": {"build": {"providers": {"namespace": []}}}}},
        ]
        for malformed in malformed_configs:
            self.assertEqual(
                github_workflow_config.workflow_provider_config(
                    malformed,
                    "build",
                    "namespace",
                ),
                {},
            )

    def test_dispatch_module_resolves_config_and_cli_selector_values(self) -> None:
        config = {
            "github_actions": {
                "workflows": {
                    "build": {
                        "providers": {
                            "namespace": {
                                "runner_selector_json": '"namespace-default"',
                                "linux_runner_selector_json": '"namespace-linux"',
                            },
                        },
                    },
                },
            },
        }

        self.assertEqual(
            github_workflow_dispatch.resolve_workflow_runner_selector_json(
                config,
                "build",
                "namespace",
            ),
            '"namespace-default"',
        )
        self.assertEqual(
            github_workflow_dispatch.resolve_workflow_dispatch_field_values(
                config,
                "build",
                "namespace",
                ("linux_runner_selector_json", "windows_runner_selector_json"),
            ),
            {"linux_runner_selector_json": '"namespace-linux"'},
        )
        self.assertEqual(
            github_workflow_dispatch.resolve_cli_dispatch_field_values(
                SimpleNamespace(
                    linux_runner_selector_json='"linux-cli"',
                    windows_runner_selector_json=None,
                    macos_runner_selector_json=None,
                ),
                ("linux_runner_selector_json",),
            ),
            {"linux_runner_selector_json": '"linux-cli"'},
        )

    def test_provider_module_resolves_defaults_and_unsupported_overrides(self) -> None:
        self.assertEqual(
            github_workflow_provider.resolve_default_provider_for_workflow(
                {"provider": "namespace"},
                "build",
            ),
            ("namespace", "github_actions.defaults.provider"),
        )
        self.assertEqual(
            github_workflow_provider.resolve_default_provider_for_workflow(
                {"provider": "namespace"},
                "validate",
            )[0],
            "github-hosted",
        )
        with self.assertRaisesRegex(ValueError, "does not support provider"):
            github_workflow_provider.resolve_default_provider_for_workflow(
                {},
                "validate",
                explicit_provider="namespace",
            )


if __name__ == "__main__":
    unittest.main()
