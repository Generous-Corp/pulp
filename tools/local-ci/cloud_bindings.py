"""Bindings from the local_ci facade to cloud/GitHub helpers."""

from __future__ import annotations

from collections.abc import Mapping
from functools import update_wrapper
from typing import Any

from binding_utils import binding as _binding


CLOUD_HELPER_EXPORTS = (
    "billing_note_text",
    "billing_period_window",
    "cloud_record_sort_key",
    "cloud_run_path",
    "compare_cloud_providers",
    "duration_between",
    "enrich_cloud_record_provider_metadata",
    "estimate_billing_period_totals",
    "estimate_cloud_record_cost",
    "estimate_github_hosted_cost",
    "estimate_namespace_cost",
    "fetch_github_repo_actions_billing_summary",
    "filter_cloud_records",
    "find_cloud_record",
    "format_currency_amount",
    "format_duration_secs",
    "format_memory_megabytes",
    "gh_api_json",
    "gh_auth_status_text",
    "gh_current_login",
    "gh_find_dispatched_run",
    "gh_repo_name",
    "gh_repo_variables",
    "gh_token_scopes",
    "infer_job_os",
    "iter_year_months",
    "load_cloud_record",
    "load_result",
    "match_namespace_shape_rate",
    "median_or_none",
    "namespace_instance_duration_secs",
    "namespace_instances_for_run",
    "normalize_cloud_record",
    "normalize_github_timestamp",
    "normalize_namespace_instance",
    "nsc_available",
    "nsc_instance_history",
    "nsc_logged_in",
    "nsc_run",
    "nsc_version",
    "nsc_workspace_info",
    "parse_colon_separated_fields",
    "parse_iso_date",
    "parse_iso_datetime",
    "parse_optional_bool",
    "parse_rate_value",
    "print_billing_period_summary",
    "print_cloud_field_detail",
    "print_github_repo_billing_summary",
    "print_namespace_setup_help",
    "print_namespace_usage_summary",
    "provider_billing_note_text",
    "recommend_cloud_provider",
    "refresh_cloud_record",
    "render_selector_value",
    "resolve_billing_settings",
    "resolve_github_repository",
    "save_cloud_record",
    "summarize_cloud_timing",
    "summarize_namespace_usage",
    "summarize_runner_selector",
    "update_cloud_record_from_run",
)


def cmd_cloud_workflows(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_workflows(args)


def cmd_cloud_defaults(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_defaults(args)


def cmd_cloud_history(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_history(args)


def cmd_cloud_compare(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_compare(args)


def cmd_cloud_recommend(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_recommend(args)


def cmd_cloud_run(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_run(args)


def cmd_cloud_status(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_status(args)


def cmd_cloud_namespace_doctor(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_namespace_doctor(args)


def cmd_cloud_namespace_setup(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_namespace_setup(args)


def gh_available(bindings: Mapping[str, Any]) -> bool:
    return _binding(bindings, "_cloud").gh_available()


def gh_workflow_dispatch(
    bindings: Mapping[str, Any],
    repository: str,
    workflow_file: str,
    ref: str,
    fields: dict[str, str],
) -> None:
    return _binding(bindings, "_cloud").gh_workflow_dispatch(repository, workflow_file, ref, fields)


def gh_run_view(bindings: Mapping[str, Any], repository: str, run_id: int) -> dict | None:
    return _binding(bindings, "_cloud").gh_run_view(repository, run_id)


def gh_pr_create(bindings: Mapping[str, Any], branch: str, base: str = "main") -> int | None:
    return _binding(bindings, "_cloud").gh_pr_create(branch, base)


def gh_pr_comment(bindings: Mapping[str, Any], pr_number: int, body: str) -> bool:
    return _binding(bindings, "_cloud").gh_pr_comment(pr_number, body)


def gh_pr_merge(bindings: Mapping[str, Any], pr_number: int, method: str = "squash") -> bool:
    return _binding(bindings, "_cloud").gh_pr_merge(pr_number, method)


def gh_pr_list_open(bindings: Mapping[str, Any]) -> list[dict]:
    return _binding(bindings, "_cloud").gh_pr_list_open()


def gh_pr_head(bindings: Mapping[str, Any], pr_ref: str) -> tuple[int, str, str] | None:
    return _binding(bindings, "_cloud").gh_pr_head(pr_ref)


def list_cloud_records(bindings: Mapping[str, Any], limit: int | None = None) -> list[dict]:
    return _binding(bindings, "_cloud").list_cloud_records(limit=limit)


def cloud_record_summary(bindings: Mapping[str, Any], record: dict, config: dict | None = None) -> str:
    return _binding(bindings, "_cloud").cloud_record_summary(record, config)


def format_ci_comment(bindings: Mapping[str, Any], result: dict) -> str:
    return _binding(bindings, "_cloud").format_ci_comment(result)


def open_pr_list_lines(bindings: Mapping[str, Any], prs: list[dict]) -> list[str]:
    return _binding(bindings, "_cloud").open_pr_list_lines(prs)


def bind_cloud_helper(bindings: Mapping[str, Any], name: str):
    helper = getattr(_binding(bindings, "_cloud"), name)

    def _helper(*args, **kwargs):
        return getattr(_binding(bindings, "_cloud"), name)(*args, **kwargs)

    return update_wrapper(_helper, helper)


def install_cloud_helpers(bindings: dict[str, Any], names: tuple[str, ...] = CLOUD_HELPER_EXPORTS) -> None:
    for name in names:
        bindings[name] = bind_cloud_helper(bindings, name)
