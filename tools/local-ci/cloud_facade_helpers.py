"""Dependency wiring helpers for the cloud compatibility facade."""
from __future__ import annotations

from collections.abc import Callable
from datetime import datetime
from pathlib import Path


def save_cloud_record_with_deps(
    record: dict,
    *,
    save_cloud_record_fn: Callable[..., Path],
    ensure_state_dirs_fn: Callable[[], None],
    cloud_run_path_fn: Callable[[str], Path],
    atomic_write_text_fn: Callable[[Path, str], None],
) -> Path:
    return save_cloud_record_fn(
        record,
        ensure_state_dirs_fn=ensure_state_dirs_fn,
        cloud_run_path_fn=cloud_run_path_fn,
        atomic_write_text_fn=atomic_write_text_fn,
    )


def list_cloud_records_with_deps(
    *,
    limit: int | None,
    list_cloud_records_fn: Callable[..., list[dict]],
    ensure_state_dirs_fn: Callable[[], None],
    cloud_runs_dir_fn: Callable[[], Path],
    load_cloud_record_fn: Callable[[Path], dict],
) -> list[dict]:
    return list_cloud_records_fn(
        limit=limit,
        ensure_state_dirs_fn=ensure_state_dirs_fn,
        cloud_runs_dir_fn=cloud_runs_dir_fn,
        load_cloud_record_fn=load_cloud_record_fn,
    )


def cloud_record_summary_with_deps(
    record: dict,
    config: dict | None,
    *,
    cloud_record_summary_fn: Callable[..., str],
    estimate_cloud_record_cost_fn: Callable[[dict, dict | None], dict],
    format_currency_amount_fn: Callable[..., str],
) -> str:
    return cloud_record_summary_fn(
        record,
        config,
        estimate_cloud_record_cost_fn=estimate_cloud_record_cost_fn,
        format_currency_amount_fn=format_currency_amount_fn,
    )


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


def update_cloud_record_from_run_with_deps(
    record: dict,
    snapshot: dict,
    *,
    provider_resolved: str | None,
    update_cloud_record_from_run_fn: Callable[..., dict],
    now_iso_fn: Callable[[], str],
) -> dict:
    return update_cloud_record_from_run_fn(
        record,
        snapshot,
        provider_resolved=provider_resolved,
        now_iso_fn=now_iso_fn,
    )
