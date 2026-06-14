#!/usr/bin/env python3
"""Tests for split cloud reporting command modules."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import unittest

from module_test_utils import load_module_from_path


def load_module(name: str):
    return load_module_from_path(Path(__file__).with_name(f"{name}.py"), add_module_dir=True)


class CloudReportingCommandModuleTests(unittest.TestCase):
    def test_history_command_handles_empty_filtered_records(self) -> None:
        mod = load_module("cloud_reporting_history")
        lines: list[str] = []

        exit_code = mod.cmd_cloud_history(
            SimpleNamespace(workflow="build", provider=None, limit=10),
            load_optional_config_fn=lambda: {"cfg": True},
            filter_cloud_records_fn=lambda records, **_kwargs: [],
            list_cloud_records_fn=lambda **_kwargs: [{"dispatch_id": "abc"}],
            cloud_history_lines_fn=lambda *_args, **_kwargs: [],
            cloud_record_summary_fn=lambda _record, _config: "",
            print_billing_period_summary_fn=lambda *_args, **_kwargs: None,
            estimate_billing_period_totals_fn=lambda _records, _config: {},
            resolve_github_actions_settings_fn=lambda _config: {},
            resolve_github_repository_fn=lambda _settings: "danielraffel/pulp",
            fetch_github_repo_actions_billing_summary_fn=lambda _repo, _config: {},
            print_github_repo_billing_summary_fn=lambda _summary: None,
            print_fn=lines.append,
        )

        self.assertEqual(exit_code, 0)
        self.assertEqual(lines, ["No tracked cloud runs found."])

    def test_compare_and_recommend_commands_delegate_to_provider_helpers(self) -> None:
        mod = load_module("cloud_reporting_compare")
        compare_lines: list[str] = []
        compare_periods: list[dict] = []

        compare_exit = mod.cmd_cloud_compare(
            SimpleNamespace(workflow=None),
            load_optional_config_fn=lambda: {"cfg": True},
            resolve_github_actions_settings_fn=lambda _config: {"workflow": "coverage"},
            compare_cloud_providers_fn=lambda _records, _config, **_kwargs: [{"provider": "namespace", "period": {"total": 1}}],
            list_cloud_records_fn=lambda **_kwargs: [{"dispatch_id": "abc"}],
            cloud_compare_summary_line_fn=lambda summary: f"provider={summary['provider']}",
            print_billing_period_summary_fn=lambda period, **_kwargs: compare_periods.append(period),
            print_fn=compare_lines.append,
        )

        recommend_lines: list[str] = []
        recommend_exit = mod.cmd_cloud_recommend(
            SimpleNamespace(workflow="build"),
            load_optional_config_fn=lambda: {"cfg": True},
            resolve_github_actions_settings_fn=lambda _config: {},
            recommend_cloud_provider_fn=lambda _records, _config, **_kwargs: ("github-hosted", "fastest"),
            list_cloud_records_fn=lambda **_kwargs: [{"dispatch_id": "abc"}],
            cloud_recommend_lines_fn=lambda workflow, provider, reason: [f"{workflow}:{provider}:{reason}"],
            print_fn=recommend_lines.append,
        )

        self.assertEqual(compare_exit, 0)
        self.assertIn("Cloud compare: workflow=coverage\n", compare_lines)
        self.assertIn("provider=namespace", compare_lines)
        self.assertEqual(compare_periods, [{"total": 1}])
        self.assertEqual(recommend_exit, 0)
        self.assertEqual(recommend_lines, ["build:github-hosted:fastest"])

    def test_workflows_and_defaults_commands_render_injected_lines(self) -> None:
        workflows = load_module("cloud_reporting_workflows")
        defaults = load_module("cloud_reporting_defaults")

        workflow_lines: list[str] = []
        self.assertEqual(
            workflows.cmd_cloud_workflows(
                SimpleNamespace(),
                builtin_github_workflows={"build": {}},
                cloud_workflow_lines_fn=lambda workflows: [f"workflow-count={len(workflows)}"],
                print_fn=workflow_lines.append,
            ),
            0,
        )

        defaults_lines: list[str] = []
        self.assertEqual(
            defaults.cmd_cloud_defaults(
                SimpleNamespace(),
                load_optional_config_fn=lambda: {"cfg": True},
                github_actions_settings_for_display_fn=lambda _config: {"repository": "display/repo"},
                resolve_github_actions_settings_fn=lambda _config: {"repository": "resolved/repo"},
                resolve_github_repository_fn=lambda settings: settings["repository"],
                gh_available_fn=lambda: True,
                gh_repo_variables_fn=lambda repository: {"repo": repository},
                cloud_defaults_lines_fn=lambda _config, _settings, **kwargs: [
                    kwargs["repository"],
                    kwargs["repository_variables"]["repo"],
                ],
                print_fn=defaults_lines.append,
            ),
            0,
        )

        self.assertEqual(workflow_lines, ["workflow-count=1"])
        self.assertEqual(defaults_lines, ["resolved/repo", "resolved/repo"])

    def test_status_command_reports_missing_record_and_recent_empty_state(self) -> None:
        mod = load_module("cloud_reporting_status")

        recent_lines: list[str] = []
        self.assertEqual(
            mod.cmd_cloud_status(
                SimpleNamespace(identifier=None, limit=5, refresh=False),
                load_optional_config_fn=lambda: {},
                list_cloud_records_fn=lambda **_kwargs: [],
                cloud_recent_status_lines_fn=lambda *_args, **_kwargs: [],
                cloud_record_summary_fn=lambda _record, _config: "",
                print_billing_period_summary_fn=lambda *_args, **_kwargs: None,
                estimate_billing_period_totals_fn=lambda _records, _config: {},
                find_cloud_record_fn=lambda _records, _identifier: None,
                gh_available_fn=lambda: False,
                resolve_github_repository_fn=lambda _settings: "",
                resolve_github_actions_settings_fn=lambda _config: {},
                refresh_cloud_record_fn=lambda *_args, **_kwargs: {},
                normalize_cloud_record_fn=lambda record: record,
                estimate_cloud_record_cost_fn=lambda _record, _config: {},
                cloud_status_detail_lines_fn=lambda _record: [],
                print_namespace_usage_summary_fn=lambda _record: None,
                cloud_status_job_lines_fn=lambda _record: [],
                print_fn=recent_lines.append,
            ),
            0,
        )

        missing_lines: list[str] = []
        self.assertEqual(
            mod.cmd_cloud_status(
                SimpleNamespace(identifier="missing", limit=5, refresh=False),
                load_optional_config_fn=lambda: {},
                list_cloud_records_fn=lambda **_kwargs: [{"dispatch_id": "abc"}],
                cloud_recent_status_lines_fn=lambda *_args, **_kwargs: [],
                cloud_record_summary_fn=lambda _record, _config: "",
                print_billing_period_summary_fn=lambda *_args, **_kwargs: None,
                estimate_billing_period_totals_fn=lambda _records, _config: {},
                find_cloud_record_fn=lambda _records, _identifier: None,
                gh_available_fn=lambda: False,
                resolve_github_repository_fn=lambda _settings: "",
                resolve_github_actions_settings_fn=lambda _config: {},
                refresh_cloud_record_fn=lambda *_args, **_kwargs: {},
                normalize_cloud_record_fn=lambda record: record,
                estimate_cloud_record_cost_fn=lambda _record, _config: {},
                cloud_status_detail_lines_fn=lambda _record: [],
                print_namespace_usage_summary_fn=lambda _record: None,
                cloud_status_job_lines_fn=lambda _record: [],
                print_fn=missing_lines.append,
            ),
            1,
        )

        self.assertEqual(recent_lines, ["No tracked cloud runs yet."])
        self.assertEqual(missing_lines, ["No matching cloud runs found."])


if __name__ == "__main__":
    unittest.main()
