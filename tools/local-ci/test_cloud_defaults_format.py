#!/usr/bin/env python3
"""Tests for cloud defaults output line helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cloud_defaults_format.py", add_module_dir=True)


class CloudDefaultsFormatTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cloud_field_detail_line_renders_values_sources_and_unset_notes(self) -> None:
        self.assertEqual(
            self.mod.cloud_field_detail_line(
                "runner_selector_json",
                '["self-hosted","Linux","ARM64"]',
                "repo variable PULP_RUNS_ON_JSON",
            ),
            "    runner_selector_json: self-hosted,Linux,ARM64 (repo variable PULP_RUNS_ON_JSON)",
        )
        self.assertEqual(
            self.mod.cloud_field_detail_line("runner_provider", "", unset_note="choose per workflow"),
            "    runner_provider: unset (choose per workflow)",
        )
        self.assertEqual(
            self.mod.cloud_field_detail_line("runner_provider", ""),
            "    runner_provider: unset",
        )

    def test_cloud_defaults_lines_render_header_and_workflow_summaries(self) -> None:
        config = {
            "github_actions": {
                "defaults": {
                    "provider": "namespace",
                    "runner_selector_json": '"namespace-profile-default"',
                },
                "workflows": {
                    "docs-check": {
                        "providers": {
                            "namespace": {
                                "runner_selector_json": '"namespace-profile-docs-config"',
                            }
                        }
                    }
                },
            }
        }
        settings = {
            "workflow": "build",
            "provider": "namespace",
            "runner_selector_json": '"namespace-profile-default"',
        }
        lines = self.mod.cloud_defaults_lines(
            config,
            settings,
            repository="danielraffel/pulp",
            repository_variables={
                "PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON": '"namespace-profile-docs-repo"',
            },
        )

        self.assertEqual(
            lines[:7],
            [
                "Cloud defaults:",
                "",
                "  repository: danielraffel/pulp",
                "  configured default workflow: build",
                "  configured default provider: namespace",
                "  billing estimates: USD period-day=1 (estimated; verify provider pricing)",
                "  provider billing truth: disabled (opt-in; off by default)",
            ],
        )
        self.assertIn("  build: Build and Test (build.yml)", lines)
        self.assertIn("    default provider: namespace (github_actions.defaults.provider)", lines)
        self.assertIn("    linux_runner_selector_json: unset", lines)
        self.assertIn(
            "    macos_runner_selector_json: unset (macOS stays local-first unless a config default or one-off override is set)",
            lines,
        )
        self.assertIn("  docs-check: Docs Consistency (docs-check.yml)", lines)
        self.assertIn(
            "    runner_selector_json: namespace-profile-docs-config (config github_actions.workflows.docs-check.providers.namespace.runner_selector_json)",
            lines,
        )

    def test_cloud_defaults_lines_render_unresolved_repository_note(self) -> None:
        lines = self.mod.cloud_defaults_lines(
            {},
            {"workflow": "build", "provider": "github-hosted"},
            repository_note="github_actions.defaults.wait_poll_secs must be an integer.",
        )
        self.assertEqual(lines[2], "  repository: unresolved")
        self.assertEqual(lines[3], "  note: github_actions.defaults.wait_poll_secs must be an integer.")


if __name__ == "__main__":
    unittest.main()
