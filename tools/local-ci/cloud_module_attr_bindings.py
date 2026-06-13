"""Installer for cloud helpers that remain direct module-attribute bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_module_attrs


CLOUD_BILLING_EXPORTS = (
    "billing_note_text",
    "billing_period_window",
    "compare_cloud_providers",
    "duration_between",
    "estimate_billing_period_totals",
    "estimate_cloud_record_cost",
    "estimate_github_hosted_cost",
    "estimate_namespace_cost",
    "fetch_github_repo_actions_billing_summary",
    "format_currency_amount",
    "iter_year_months",
    "match_namespace_shape_rate",
    "namespace_instance_duration_secs",
    "namespace_instances_for_run",
    "parse_iso_date",
    "parse_iso_datetime",
    "parse_rate_value",
    "print_billing_period_summary",
    "print_github_repo_billing_summary",
    "print_namespace_usage_summary",
    "provider_billing_note_text",
    "resolve_billing_settings",
    "summarize_cloud_timing",
    "summarize_namespace_usage",
)

CLOUD_RECORD_STORE_EXPORTS = (
    "cloud_record_sort_key",
    "cloud_run_path",
    "enrich_cloud_record_provider_metadata",
    "filter_cloud_records",
    "find_cloud_record",
    "load_cloud_record",
    "load_result",
    "normalize_cloud_record",
    "refresh_cloud_record",
    "save_cloud_record",
    "update_cloud_record_from_run",
)

CLOUD_GITHUB_MODULE_EXPORTS = (
    "gh_api_json",
    "gh_auth_status_text",
    "gh_current_login",
    "gh_find_dispatched_run",
    "gh_repo_name",
    "gh_repo_variables",
    "gh_token_scopes",
    "resolve_github_repository",
)

CLOUD_NAMESPACE_EXPORTS = (
    "infer_job_os",
    "normalize_github_timestamp",
    "normalize_namespace_instance",
    "nsc_available",
    "nsc_instance_history",
    "nsc_logged_in",
    "nsc_run",
    "nsc_version",
    "nsc_workspace_info",
    "parse_colon_separated_fields",
    "parse_optional_bool",
    "print_namespace_setup_help",
)

CLOUD_FORMAT_EXPORTS = (
    "format_duration_secs",
    "format_memory_megabytes",
    "median_or_none",
    "print_cloud_field_detail",
    "recommend_cloud_provider",
    "render_selector_value",
    "summarize_runner_selector",
)

CLOUD_MODULE_ATTR_EXPORTS = (
    *CLOUD_BILLING_EXPORTS,
    *CLOUD_RECORD_STORE_EXPORTS,
    *CLOUD_GITHUB_MODULE_EXPORTS,
    *CLOUD_NAMESPACE_EXPORTS,
    *CLOUD_FORMAT_EXPORTS,
)


def install_cloud_module_attr_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_MODULE_ATTR_EXPORTS,
) -> None:
    install_module_attrs(bindings, "_cloud", names)
