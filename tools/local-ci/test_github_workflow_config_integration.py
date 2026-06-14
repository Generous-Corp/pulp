#!/usr/bin/env python3
"""Facade-level GitHub Actions workflow config integration tests."""

from __future__ import annotations

import unittest
from argparse import Namespace
from pathlib import Path

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_local_ci_github_workflow_config_integration", add_module_dir=True)


class GithubWorkflowConfigIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_github_workflow_defaults_cover_config_repo_and_cli_edges(self) -> None:
        config = {
            "github_actions": {
                "repository": " danielraffel/pulp ",
                "defaults": {
                    "workflow": "docs-check",
                    "provider": "namespace",
                    "wait_poll_secs": "7",
                    "match_timeout_secs": "11",
                },
                "workflows": {
                    "docs-check": {
                        "providers": {
                            "namespace": {
                                "runner_selector_json": '["namespace-profile", "macos"]',
                            }
                        }
                    },
                    "build": {
                        "providers": {
                            "namespace": {
                                "linux_runner_selector_json": '"linux-large"',
                                "windows_runner_selector_json": "",
                            }
                        }
                    },
                },
            }
        }
        settings = self.mod.resolve_github_actions_settings(config)
        self.assertEqual(settings["repository"], "danielraffel/pulp")
        self.assertEqual(settings["workflow"], "docs-check")
        self.assertEqual(settings["provider"], "namespace")
        self.assertEqual(settings["wait_poll_secs"], 7)
        self.assertEqual(settings["match_timeout_secs"], 11)

        summary = self.mod.summarize_workflow_provider_defaults(
            config,
            {"PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": '"windows-large"'},
            settings,
            "docs-check",
        )
        self.assertEqual(summary["provider"], "namespace")
        self.assertEqual(summary["provider_source"], "github_actions.defaults.provider")
        self.assertEqual(summary["selector_input"], "runner_selector_json")
        self.assertEqual(summary["selector_value"], '["namespace-profile", "macos"]')
        self.assertEqual(
            self.mod.resolve_default_provider_for_workflow({"provider": "namespace"}, "validate"),
            ("github-hosted", "workflow fallback (default provider 'namespace' unsupported)"),
        )

        fields, sources = self.mod.resolve_workflow_dispatch_defaults(
            config,
            {
                "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": '"windows-large"',
                "PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON": '["macos", "arm64"]',
            },
            "build",
            "namespace",
            ("linux_runner_selector_json", "windows_runner_selector_json", "macos_runner_selector_json"),
        )
        self.assertEqual(
            fields,
            {
                "linux_runner_selector_json": '"linux-large"',
                "windows_runner_selector_json": '"windows-large"',
                "macos_runner_selector_json": '["macos", "arm64"]',
            },
        )
        self.assertIn("config github_actions.workflows.build.providers.namespace", sources["linux_runner_selector_json"])
        self.assertEqual(sources["windows_runner_selector_json"], "repo variable PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON")

        args = Namespace(
            linux_runner_selector_json='"linux-cli"',
            windows_runner_selector_json=None,
            macos_runner_selector_json=None,
        )
        self.assertEqual(
            self.mod.resolve_cli_dispatch_field_values(args, ("linux_runner_selector_json",)),
            {"linux_runner_selector_json": '"linux-cli"'},
        )
        with self.assertRaisesRegex(ValueError, "not supported"):
            self.mod.resolve_cli_dispatch_field_values(args, ())
        with self.assertRaisesRegex(ValueError, "must be valid JSON"):
            self.mod.normalize_runs_on_json("{", setting_name="bad.selector")
        with self.assertRaisesRegex(ValueError, "must decode to a string or array"):
            self.mod.normalize_runs_on_json('{"runs-on": "ubuntu"}', setting_name="bad.selector")
        with self.assertRaisesRegex(ValueError, "must be positive"):
            self.mod.resolve_github_actions_settings({"github_actions": {"defaults": {"wait_poll_secs": 0}}})


if __name__ == "__main__":
    unittest.main()
