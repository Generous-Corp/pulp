"""Command wiring helpers for the cloud compatibility facade."""
from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def _dep(deps: Mapping[str, Any], name: str) -> Any:
    return deps[name]


def cmd_cloud_namespace_doctor_with_deps(args: Any, deps: Mapping[str, Any]) -> int:
    return _dep(deps, "_cmd_cloud_namespace_doctor")(
        args,
        nsc_version_fn=_dep(deps, "nsc_version"),
        nsc_logged_in_fn=_dep(deps, "nsc_logged_in"),
        nsc_workspace_info_fn=_dep(deps, "nsc_workspace_info"),
        print_namespace_setup_help_fn=_dep(deps, "print_namespace_setup_help"),
    )


def cmd_cloud_namespace_setup_with_deps(args: Any, deps: Mapping[str, Any]) -> int:
    return _dep(deps, "_cmd_cloud_namespace_setup")(
        args,
        nsc_available_fn=_dep(deps, "nsc_available"),
        nsc_logged_in_fn=_dep(deps, "nsc_logged_in"),
        nsc_run_fn=_dep(deps, "nsc_run"),
        cmd_cloud_namespace_doctor_fn=_dep(deps, "cmd_cloud_namespace_doctor"),
        print_namespace_setup_help_fn=_dep(deps, "print_namespace_setup_help"),
    )


def cmd_cloud_history_with_deps(args: Any, deps: Mapping[str, Any]) -> int:
    return _dep(deps, "_cmd_cloud_history")(
        args,
        load_optional_config_fn=_dep(deps, "_load_optional_config"),
        filter_cloud_records_fn=_dep(deps, "filter_cloud_records"),
        list_cloud_records_fn=_dep(deps, "list_cloud_records"),
        cloud_history_lines_fn=_dep(deps, "cloud_history_lines"),
        cloud_record_summary_fn=_dep(deps, "cloud_record_summary"),
        print_billing_period_summary_fn=_dep(deps, "print_billing_period_summary"),
        estimate_billing_period_totals_fn=_dep(deps, "estimate_billing_period_totals"),
        resolve_github_actions_settings_fn=_dep(deps, "resolve_github_actions_settings"),
        resolve_github_repository_fn=_dep(deps, "resolve_github_repository"),
        fetch_github_repo_actions_billing_summary_fn=_dep(deps, "fetch_github_repo_actions_billing_summary"),
        print_github_repo_billing_summary_fn=_dep(deps, "print_github_repo_billing_summary"),
    )


def cmd_cloud_compare_with_deps(args: Any, deps: Mapping[str, Any]) -> int:
    return _dep(deps, "_cmd_cloud_compare")(
        args,
        load_optional_config_fn=_dep(deps, "_load_optional_config"),
        resolve_github_actions_settings_fn=_dep(deps, "resolve_github_actions_settings"),
        compare_cloud_providers_fn=_dep(deps, "compare_cloud_providers"),
        list_cloud_records_fn=_dep(deps, "list_cloud_records"),
        cloud_compare_summary_line_fn=_dep(deps, "cloud_compare_summary_line"),
        print_billing_period_summary_fn=_dep(deps, "print_billing_period_summary"),
    )


def cmd_cloud_recommend_with_deps(args: Any, deps: Mapping[str, Any]) -> int:
    return _dep(deps, "_cmd_cloud_recommend")(
        args,
        load_optional_config_fn=_dep(deps, "_load_optional_config"),
        resolve_github_actions_settings_fn=_dep(deps, "resolve_github_actions_settings"),
        recommend_cloud_provider_fn=_dep(deps, "recommend_cloud_provider"),
        list_cloud_records_fn=_dep(deps, "list_cloud_records"),
        cloud_recommend_lines_fn=_dep(deps, "cloud_recommend_lines"),
    )


def cmd_cloud_workflows_with_deps(args: Any, deps: Mapping[str, Any]) -> int:
    return _dep(deps, "_cmd_cloud_workflows")(
        args,
        builtin_github_workflows=_dep(deps, "BUILTIN_GITHUB_WORKFLOWS"),
        cloud_workflow_lines_fn=_dep(deps, "cloud_workflow_lines"),
    )


def cmd_cloud_defaults_with_deps(args: Any, deps: Mapping[str, Any]) -> int:
    return _dep(deps, "_cmd_cloud_defaults")(
        args,
        load_optional_config_fn=_dep(deps, "_load_optional_config"),
        github_actions_settings_for_display_fn=_dep(deps, "github_actions_settings_for_display"),
        resolve_github_actions_settings_fn=_dep(deps, "resolve_github_actions_settings"),
        resolve_github_repository_fn=_dep(deps, "resolve_github_repository"),
        gh_available_fn=_dep(deps, "gh_available"),
        gh_repo_variables_fn=_dep(deps, "gh_repo_variables"),
        cloud_defaults_lines_fn=_dep(deps, "cloud_defaults_lines"),
    )


def cmd_cloud_run_with_deps(args: Any, deps: Mapping[str, Any]) -> int:
    return _dep(deps, "_cmd_cloud_run")(
        args,
        gh_available_fn=_dep(deps, "gh_available"),
        load_optional_config_fn=_dep(deps, "_load_optional_config"),
        resolve_github_actions_settings_fn=_dep(deps, "resolve_github_actions_settings"),
        resolve_github_repository_fn=_dep(deps, "resolve_github_repository"),
        builtin_github_workflows=_dep(deps, "BUILTIN_GITHUB_WORKFLOWS"),
        current_branch_fn=_dep(deps, "current_branch"),
        resolve_default_provider_for_workflow_fn=_dep(deps, "resolve_default_provider_for_workflow"),
        gh_repo_variables_fn=_dep(deps, "gh_repo_variables"),
        resolve_workflow_dispatch_defaults_fn=_dep(deps, "resolve_workflow_dispatch_defaults"),
        resolve_cli_dispatch_field_values_fn=_dep(deps, "resolve_cli_dispatch_field_values"),
        normalize_runs_on_json_fn=_dep(deps, "normalize_runs_on_json"),
        resolve_workflow_field_value_and_source_fn=_dep(deps, "resolve_workflow_field_value_and_source"),
        now_iso_fn=_dep(deps, "now_iso"),
        normalize_cloud_record_fn=_dep(deps, "normalize_cloud_record"),
        cloud_run_record_payload_fn=_dep(deps, "cloud_run_record_payload"),
        gh_current_login_fn=_dep(deps, "gh_current_login"),
        save_cloud_record_fn=_dep(deps, "save_cloud_record"),
        cloud_workflow_dispatch_fields_fn=_dep(deps, "cloud_workflow_dispatch_fields"),
        gh_workflow_dispatch_fn=_dep(deps, "gh_workflow_dispatch"),
        gh_find_dispatched_run_fn=_dep(deps, "gh_find_dispatched_run"),
        enrich_cloud_record_provider_metadata_fn=_dep(deps, "enrich_cloud_record_provider_metadata"),
        update_cloud_record_from_run_fn=_dep(deps, "update_cloud_record_from_run"),
        cloud_dispatch_lines_fn=_dep(deps, "cloud_dispatch_lines"),
        refresh_cloud_record_fn=_dep(deps, "refresh_cloud_record"),
        cloud_final_status_line_fn=_dep(deps, "cloud_final_status_line"),
    )


def cmd_cloud_status_with_deps(args: Any, deps: Mapping[str, Any]) -> int:
    return _dep(deps, "_cmd_cloud_status")(
        args,
        load_optional_config_fn=_dep(deps, "_load_optional_config"),
        list_cloud_records_fn=_dep(deps, "list_cloud_records"),
        cloud_recent_status_lines_fn=_dep(deps, "cloud_recent_status_lines"),
        cloud_record_summary_fn=_dep(deps, "cloud_record_summary"),
        print_billing_period_summary_fn=_dep(deps, "print_billing_period_summary"),
        estimate_billing_period_totals_fn=_dep(deps, "estimate_billing_period_totals"),
        find_cloud_record_fn=_dep(deps, "find_cloud_record"),
        gh_available_fn=_dep(deps, "gh_available"),
        resolve_github_repository_fn=_dep(deps, "resolve_github_repository"),
        resolve_github_actions_settings_fn=_dep(deps, "resolve_github_actions_settings"),
        refresh_cloud_record_fn=_dep(deps, "refresh_cloud_record"),
        normalize_cloud_record_fn=_dep(deps, "normalize_cloud_record"),
        estimate_cloud_record_cost_fn=_dep(deps, "estimate_cloud_record_cost"),
        cloud_status_detail_lines_fn=_dep(deps, "cloud_status_detail_lines"),
        print_namespace_usage_summary_fn=_dep(deps, "print_namespace_usage_summary"),
        cloud_status_job_lines_fn=_dep(deps, "cloud_status_job_lines"),
    )
