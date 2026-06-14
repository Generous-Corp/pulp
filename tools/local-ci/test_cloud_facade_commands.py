#!/usr/bin/env python3
"""Tests for cloud compatibility facade command wiring helpers."""

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("cloud_facade_commands.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudFacadeCommandsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _deps(self):
        calls = []

        def make_command(name):
            def command(args, **kwargs):
                calls.append((name, args, kwargs))
                return len(calls)

            return command

        deps = {
            "_cmd_cloud_namespace_doctor": make_command("namespace_doctor"),
            "_cmd_cloud_namespace_setup": make_command("namespace_setup"),
            "_cmd_cloud_history": make_command("history"),
            "_cmd_cloud_compare": make_command("compare"),
            "_cmd_cloud_recommend": make_command("recommend"),
            "_cmd_cloud_workflows": make_command("workflows"),
            "_cmd_cloud_defaults": make_command("defaults"),
            "_cmd_cloud_run": make_command("run"),
            "_cmd_cloud_status": make_command("status"),
            "BUILTIN_GITHUB_WORKFLOWS": {"build": {"file": "build.yml"}},
        }
        for name in (
            "_load_optional_config",
            "cloud_compare_summary_line",
            "cloud_defaults_lines",
            "cloud_dispatch_lines",
            "cloud_final_status_line",
            "cloud_history_lines",
            "cloud_recent_status_lines",
            "cloud_record_summary",
            "cloud_recommend_lines",
            "cloud_run_record_payload",
            "cloud_status_detail_lines",
            "cloud_status_job_lines",
            "cloud_workflow_dispatch_fields",
            "cloud_workflow_lines",
            "cmd_cloud_namespace_doctor",
            "compare_cloud_providers",
            "current_branch",
            "enrich_cloud_record_provider_metadata",
            "estimate_billing_period_totals",
            "estimate_cloud_record_cost",
            "fetch_github_repo_actions_billing_summary",
            "filter_cloud_records",
            "find_cloud_record",
            "gh_available",
            "gh_current_login",
            "gh_find_dispatched_run",
            "gh_repo_variables",
            "gh_workflow_dispatch",
            "github_actions_settings_for_display",
            "list_cloud_records",
            "normalize_cloud_record",
            "normalize_runs_on_json",
            "now_iso",
            "nsc_available",
            "nsc_logged_in",
            "nsc_run",
            "nsc_version",
            "nsc_workspace_info",
            "print_billing_period_summary",
            "print_github_repo_billing_summary",
            "print_namespace_setup_help",
            "print_namespace_usage_summary",
            "recommend_cloud_provider",
            "refresh_cloud_record",
            "resolve_cli_dispatch_field_values",
            "resolve_default_provider_for_workflow",
            "resolve_github_actions_settings",
            "resolve_github_repository",
            "resolve_workflow_dispatch_defaults",
            "resolve_workflow_field_value_and_source",
            "save_cloud_record",
            "update_cloud_record_from_run",
        ):
            deps[name] = lambda *args, **kwargs: None
        return deps, calls

    def test_command_helpers_forward_expected_facade_dependencies(self):
        deps, calls = self._deps()
        args = types.SimpleNamespace()

        cases = [
            self.mod.cmd_cloud_namespace_doctor_with_deps,
            self.mod.cmd_cloud_namespace_setup_with_deps,
            self.mod.cmd_cloud_history_with_deps,
            self.mod.cmd_cloud_compare_with_deps,
            self.mod.cmd_cloud_recommend_with_deps,
            self.mod.cmd_cloud_workflows_with_deps,
            self.mod.cmd_cloud_defaults_with_deps,
            self.mod.cmd_cloud_run_with_deps,
            self.mod.cmd_cloud_status_with_deps,
        ]
        for index, helper in enumerate(cases, start=1):
            self.assertEqual(helper(args, deps), index)

        self.assertEqual(
            [call[0] for call in calls],
            [
                "namespace_doctor",
                "namespace_setup",
                "history",
                "compare",
                "recommend",
                "workflows",
                "defaults",
                "run",
                "status",
            ],
        )
        self.assertIn("cmd_cloud_namespace_doctor_fn", calls[1][2])
        self.assertIn("fetch_github_repo_actions_billing_summary_fn", calls[2][2])
        self.assertIn("cloud_workflow_dispatch_fields_fn", calls[7][2])
        self.assertIn("cloud_status_job_lines_fn", calls[8][2])


if __name__ == "__main__":
    unittest.main()
