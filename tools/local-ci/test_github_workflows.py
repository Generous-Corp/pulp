#!/usr/bin/env python3
"""Tests for the github_workflows selector/provider helpers."""

import json
import os
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

import github_workflows



class GithubWorkflowsTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        root = self.tmpdir.name
        self.config_path = os.path.join(root, "config.json")
        self.config = {
            "github_actions": {
                "repository": "danielraffel/pulp",
                "defaults": {
                    "workflow": "build",
                    "provider": "github-hosted",
                    "wait_poll_secs": 5,
                    "match_timeout_secs": 30,
                },
                "workflows": {
                    "build": {
                        "providers": {
                            "namespace": {
                                "linux_runner_selector_json": "\"namespace-profile-default\"",
                                "windows_runner_selector_json": "\"namespace-profile-default\"",
                            }
                        }
                    },
                    "docs-check": {
                        "providers": {
                            "namespace": {
                                "runner_selector_json": "\"namespace-profile-default\""
                            }
                        }
                    },
                },
            }
        }
        self.write_config(self.config)
        self.mod = github_workflows

    def tearDown(self):
        self.tmpdir.cleanup()

    def write_config(self, config):
        with open(self.config_path, "w", encoding="utf-8") as handle:
            json.dump(config, handle)
            handle.write("\n")

    def load_optional_config(self):
        with open(self.config_path, encoding="utf-8") as handle:
            return json.load(handle)

    def test_extracted_github_workflow_helpers_resolve_sources_and_cli_overrides(self):
        config = {
            "github_actions": {
                "defaults": {"provider": "namespace"},
                "workflows": {
                    "build": {
                        "providers": {
                            "namespace": {
                                "linux_runner_selector_json": '["ns-linux"]',
                            }
                        }
                    }
                },
            }
        }
        repo_vars = {
            "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": '"ns-windows"',
        }

        defaults, sources = self.mod.resolve_workflow_dispatch_defaults(
            config,
            repo_vars,
            "build",
            "namespace",
            ["linux_runner_selector_json", "windows_runner_selector_json"],
        )
        self.assertEqual(defaults["linux_runner_selector_json"], '["ns-linux"]')
        self.assertEqual(defaults["windows_runner_selector_json"], '"ns-windows"')
        self.assertIn("config github_actions.workflows.build", sources["linux_runner_selector_json"])
        self.assertIn("repo variable", sources["windows_runner_selector_json"])

        args = SimpleNamespace(
            linux_runner_selector_json='"ubuntu-24.04"',
            windows_runner_selector_json=None,
            macos_runner_selector_json=None,
        )
        self.assertEqual(
            self.mod.resolve_cli_dispatch_field_values(args, ["linux_runner_selector_json"]),
            {"linux_runner_selector_json": '"ubuntu-24.04"'},
        )

        with self.assertRaisesRegex(ValueError, "not supported for this workflow"):
            self.mod.resolve_cli_dispatch_field_values(args, ["macos_runner_selector_json"])

        with self.assertRaisesRegex(ValueError, "Unknown workflow"):
            self.mod.resolve_default_provider_for_workflow(
                {"provider": "github-hosted"}, "missing"
            )


    def test_extracted_github_workflow_settings_validate_selectors_and_providers(self):
        config = {
            "github_actions": {
                "repository": "  danielraffel/pulp  ",
                "defaults": {
                    "workflow": "  docs-check  ",
                    "provider": "  namespace  ",
                    "wait_poll_secs": "11",
                    "match_timeout_secs": 22,
                },
                "workflows": {
                    "docs-check": {
                        "providers": {
                            "namespace": {
                                "runner_selector_json": '["namespace-profile-docs"]',
                            }
                        }
                    }
                },
            }
        }

        display = self.mod.github_actions_settings_for_display(config)
        self.assertEqual(display["repository"], "danielraffel/pulp")
        self.assertEqual(display["workflow"], "docs-check")
        self.assertEqual(display["provider"], "namespace")

        settings = self.mod.resolve_github_actions_settings(config)
        self.assertEqual(settings["wait_poll_secs"], 11)
        self.assertEqual(settings["match_timeout_secs"], 22)

        selector = self.mod.resolve_workflow_runner_selector_json(
            config, "docs-check", "namespace"
        )
        self.assertEqual(selector, '["namespace-profile-docs"]')
        self.assertEqual(
            self.mod.resolve_workflow_runner_selector_json(
                {"github_actions": {"workflows": []}}, "docs-check", "namespace"
            ),
            "",
        )

        self.assertEqual(
            self.mod.normalize_runs_on_json('"macos-15"', setting_name="selector"),
            '"macos-15"',
        )
        self.assertEqual(
            self.mod.normalize_runs_on_json('["ubuntu-24.04"]', setting_name="selector"),
            '["ubuntu-24.04"]',
        )
        with self.assertRaisesRegex(ValueError, "must be valid JSON"):
            self.mod.normalize_runs_on_json("not json", setting_name="selector")
        with self.assertRaisesRegex(ValueError, "must decode to a string or array"):
            self.mod.normalize_runs_on_json("42", setting_name="selector")
        self.assertEqual(self.mod.normalize_runs_on_json("", setting_name="selector"), "")

        with self.assertRaisesRegex(ValueError, "must be positive"):
            self.mod.resolve_github_actions_settings(
                {"github_actions": {"defaults": {"wait_poll_secs": 0}}}
            )
        with self.assertRaisesRegex(ValueError, "must be an integer"):
            self.mod.resolve_github_actions_settings(
                {"github_actions": {"defaults": {"wait_poll_secs": "slow"}}}
            )

        provider, source = self.mod.resolve_default_provider_for_workflow(
            {"provider": "namespace"}, "validate"
        )
        self.assertEqual(provider, "github-hosted")
        self.assertIn("workflow fallback", source)

        provider, source = self.mod.resolve_default_provider_for_workflow(
            {"provider": "github-hosted"}, "build", explicit_provider="namespace"
        )
        self.assertEqual((provider, source), ("namespace", "cli"))

        with self.assertRaisesRegex(ValueError, "does not support provider"):
            self.mod.resolve_default_provider_for_workflow(
                {"provider": "github-hosted"}, "validate", explicit_provider="namespace"
            )


    def test_resolve_github_actions_settings_reads_optional_config_defaults(self):
        settings = self.mod.resolve_github_actions_settings(self.load_optional_config())
        self.assertEqual(settings["repository"], "danielraffel/pulp")
        self.assertEqual(settings["workflow"], "build")
        self.assertEqual(settings["provider"], "github-hosted")
        self.assertEqual(settings["wait_poll_secs"], 5)
        self.assertEqual(settings["match_timeout_secs"], 30)


    def test_resolve_workflow_runner_selector_json_reads_docs_check_provider_default(self):
        selector = self.mod.resolve_workflow_runner_selector_json(
            self.load_optional_config(), "docs-check", "namespace"
        )
        self.assertEqual(selector, "\"namespace-profile-default\"")


    def test_resolve_default_provider_for_workflow_falls_back_to_github_hosted(self):
        provider, source = self.mod.resolve_default_provider_for_workflow(
            {"provider": "namespace"},
            "validate",
        )
        self.assertEqual(provider, "github-hosted")
        self.assertIn("workflow fallback", source)


    def test_resolve_workflow_field_value_and_source_reads_repo_variable_fallback(self):
        config = self.load_optional_config()
        del config["github_actions"]["workflows"]["docs-check"]["providers"]["namespace"]["runner_selector_json"]
        self.write_config(config)

        value, source = self.mod.resolve_workflow_field_value_and_source(
            self.load_optional_config(),
            {"PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON": "\"namespace-profile-repo-var\""},
            "docs-check",
            "namespace",
            "runner_selector_json",
        )
        self.assertEqual(value, "\"namespace-profile-repo-var\"")
        self.assertEqual(source, "repo variable PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON")


    def test_resolve_workflow_dispatch_field_values_reads_build_namespace_defaults(self):
        fields = self.mod.resolve_workflow_dispatch_field_values(
            self.load_optional_config(),
            "build",
            "namespace",
            ["linux_runner_selector_json", "windows_runner_selector_json"],
        )
        self.assertEqual(
            fields,
            {
                "linux_runner_selector_json": "\"namespace-profile-default\"",
                "windows_runner_selector_json": "\"namespace-profile-default\"",
            },
        )


    def test_workflow_selector_helpers_resolve_config_repo_and_cli_values(self):
        config = {
            "github_actions": {
                "defaults": {"provider": "namespace"},
                "workflows": {
                    "build": {
                        "providers": {
                            "namespace": {
                                "linux_runner_selector_json": '"linux-config"',
                            }
                        }
                    },
                    "docs-check": {
                        "providers": {
                            "namespace": {},
                        }
                    },
                },
            }
        }
        repo_variables = {
            "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": '"windows-repo"',
            "PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON": '["self-hosted", "macOS"]',
            "PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON": '"docs-repo"',
        }
        settings = self.mod.resolve_github_actions_settings(config)

        build_summary = self.mod.summarize_workflow_provider_defaults(
            config, repo_variables, settings, "build"
        )
        self.assertEqual(build_summary["provider"], "namespace")
        self.assertEqual(
            build_summary["dispatch_fields"],
            {
                "linux_runner_selector_json": '"linux-config"',
                "windows_runner_selector_json": '"windows-repo"',
                "macos_runner_selector_json": '["self-hosted", "macOS"]',
            },
        )
        self.assertEqual(
            build_summary["dispatch_sources"]["linux_runner_selector_json"],
            "config github_actions.workflows.build.providers.namespace.linux_runner_selector_json",
        )
        self.assertEqual(
            build_summary["dispatch_sources"]["windows_runner_selector_json"],
            "repo variable PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON",
        )

        docs_summary = self.mod.summarize_workflow_provider_defaults(
            config, repo_variables, settings, "docs-check"
        )
        self.assertEqual(docs_summary["selector_value"], '"docs-repo"')
        self.assertEqual(
            docs_summary["selector_source"],
            "repo variable PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON",
        )

        cli_fields = self.mod.resolve_cli_dispatch_field_values(
            SimpleNamespace(
                linux_runner_selector_json='"linux-cli"',
                windows_runner_selector_json=None,
                macos_runner_selector_json=None,
            ),
            ["linux_runner_selector_json"],
        )
        self.assertEqual(cli_fields, {"linux_runner_selector_json": '"linux-cli"'})
        with self.assertRaisesRegex(ValueError, "not supported"):
            self.mod.resolve_cli_dispatch_field_values(
                SimpleNamespace(
                    linux_runner_selector_json='"linux-cli"',
                    windows_runner_selector_json=None,
                    macos_runner_selector_json=None,
                ),
                [],
            )
        with self.assertRaisesRegex(ValueError, "valid JSON"):
            self.mod.resolve_cli_dispatch_field_values(
                SimpleNamespace(
                    linux_runner_selector_json="linux-cli",
                    windows_runner_selector_json=None,
                    macos_runner_selector_json=None,
                ),
                ["linux_runner_selector_json"],
            )

    def test_workflow_selector_helpers_handle_malformed_config_shapes(self):
        malformed_configs = [
            {"github_actions": {"workflows": []}},
            {"github_actions": {"workflows": {"build": []}}},
            {"github_actions": {"workflows": {"build": {"providers": []}}}},
            {"github_actions": {"workflows": {"build": {"providers": {"namespace": []}}}}},
            {
                "github_actions": {
                    "workflows": {
                        "build": {
                            "providers": {
                                "namespace": {
                                    "runner_selector_json": "   ",
                                }
                            }
                        }
                    }
                }
            },
        ]
        for config in malformed_configs:
            self.assertEqual(
                self.mod.resolve_workflow_runner_selector_json(
                    config, "build", "namespace"
                ),
                "",
            )

        self.assertEqual(
            self.mod.resolve_workflow_dispatch_field_values(
                self.load_optional_config(), "build", "namespace", []
            ),
            {},
        )
        for config in malformed_configs[:-1]:
            self.assertEqual(
                self.mod.resolve_workflow_dispatch_field_values(
                    config,
                    "build",
                    "namespace",
                    ["linux_runner_selector_json"],
                ),
                {},
            )

        value, source = self.mod.resolve_workflow_field_value_and_source(
            {},
            {},
            "build",
            "github-hosted",
            "unmapped_selector_json",
        )
        self.assertEqual((value, source), ("", ""))

        defaults, sources = self.mod.resolve_workflow_dispatch_defaults(
            {},
            {},
            "build",
            "namespace",
            ["linux_runner_selector_json"],
        )
        self.assertEqual(defaults, {})
        self.assertEqual(sources, {})

        with mock.patch.object(
            github_workflows,
            "resolve_workflow_field_value_and_source",
            return_value=('"forced"', ""),
        ):
            defaults, sources = self.mod.resolve_workflow_dispatch_defaults(
                {},
                {},
                "build",
                "namespace",
                ["linux_runner_selector_json"],
            )
        self.assertEqual(defaults, {"linux_runner_selector_json": '"forced"'})
        self.assertEqual(sources, {})



if __name__ == "__main__":
    unittest.main()
