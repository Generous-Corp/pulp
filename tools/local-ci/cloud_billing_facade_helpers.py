"""Cloud billing and provider metadata facade dependency wiring helpers."""

from __future__ import annotations

from collections.abc import Callable
from datetime import datetime


def estimate_billing_period_totals_with_deps(
    records: list[dict],
    config: dict | None,
    *,
    provider: str | None,
    estimate_billing_period_totals_fn: Callable[..., dict],
    billing_period_window_fn: Callable[..., tuple[datetime, datetime]],
) -> dict:
    return estimate_billing_period_totals_fn(
        records,
        config,
        provider=provider,
        period_window_func=billing_period_window_fn,
    )


def fetch_github_repo_actions_billing_summary_with_deps(
    repository: str,
    config: dict | None,
    *,
    fetch_github_repo_actions_billing_summary_fn: Callable[..., dict],
    resolve_billing_settings_fn: Callable[[dict | None], dict],
    gh_available_fn: Callable[[], bool],
    gh_api_json_fn: Callable[..., tuple[dict | None, str]],
    billing_period_window_fn: Callable[..., tuple[datetime, datetime]],
    iter_year_months_fn: Callable[[datetime, datetime], list[tuple[int, int]]],
    gh_token_scopes_fn: Callable[[], set[str]],
    parse_iso_date_fn: Callable[[str | None], object],
    provider_billing_note_text_fn: Callable[[], str],
) -> dict:
    return fetch_github_repo_actions_billing_summary_fn(
        repository,
        config,
        resolve_billing_settings_fn=resolve_billing_settings_fn,
        gh_available_fn=gh_available_fn,
        gh_api_json_fn=gh_api_json_fn,
        billing_period_window_fn=billing_period_window_fn,
        iter_year_months_fn=iter_year_months_fn,
        gh_token_scopes_fn=gh_token_scopes_fn,
        parse_iso_date_fn=parse_iso_date_fn,
        provider_billing_note_text_fn=provider_billing_note_text_fn,
    )


def enrich_cloud_record_provider_metadata_with_deps(
    record: dict,
    *,
    enrich_cloud_record_provider_metadata_fn: Callable[..., dict],
    normalize_cloud_record_fn: Callable[[dict], dict],
    nsc_logged_in_fn: Callable[[], bool],
    namespace_instances_for_run_fn: Callable[[str, int], list[dict]],
    summarize_namespace_usage_fn: Callable[[list[dict]], dict],
) -> dict:
    return enrich_cloud_record_provider_metadata_fn(
        record,
        normalize_cloud_record_fn=normalize_cloud_record_fn,
        nsc_logged_in_fn=nsc_logged_in_fn,
        namespace_instances_for_run_fn=namespace_instances_for_run_fn,
        summarize_namespace_usage_fn=summarize_namespace_usage_fn,
    )
