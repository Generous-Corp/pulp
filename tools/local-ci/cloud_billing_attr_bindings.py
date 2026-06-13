"""Installer for cloud billing helpers that remain direct module attributes."""

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


def install_cloud_billing_attr_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_BILLING_EXPORTS,
) -> None:
    install_module_attrs(bindings, "_cloud", names)
