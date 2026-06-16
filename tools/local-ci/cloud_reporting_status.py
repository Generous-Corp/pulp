"""Cloud status command orchestration."""
from __future__ import annotations

from collections.abc import Callable
import argparse


def cmd_cloud_status(
    args: argparse.Namespace,
    *,
    load_optional_config_fn: Callable[[], dict],
    list_cloud_records_fn: Callable[..., list[dict]],
    cloud_recent_status_lines_fn: Callable[..., list[str]],
    cloud_record_summary_fn: Callable[[dict, dict | None], str],
    print_billing_period_summary_fn: Callable[..., None],
    estimate_billing_period_totals_fn: Callable[[list[dict], dict | None], dict],
    find_cloud_record_fn: Callable[[list[dict], str], dict | None],
    gh_available_fn: Callable[[], bool],
    resolve_github_repository_fn: Callable[[dict], str],
    resolve_github_actions_settings_fn: Callable[[dict], dict],
    refresh_cloud_record_fn: Callable[..., dict],
    normalize_cloud_record_fn: Callable[[dict], dict],
    estimate_cloud_record_cost_fn: Callable[[dict, dict | None], dict],
    cloud_status_detail_lines_fn: Callable[[dict], list[str]],
    print_namespace_usage_summary_fn: Callable[[dict], None],
    cloud_status_job_lines_fn: Callable[[dict], list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    config = load_optional_config_fn()
    if args.identifier is None:
        records = list_cloud_records_fn(limit=args.limit)
        if not records:
            print_fn("No tracked cloud runs yet.")
            return 0
        for line in cloud_recent_status_lines_fn(records, config, summary_fn=cloud_record_summary_fn):
            print_fn(line)
        print_billing_period_summary_fn(
            estimate_billing_period_totals_fn(list_cloud_records_fn(limit=None), config)
        )
        return 0

    try:
        record = find_cloud_record_fn(list_cloud_records_fn(), args.identifier)
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    if record is None:
        print_fn("No matching cloud runs found.")
        return 1

    if args.refresh:
        if not gh_available_fn():
            print_fn("Error: gh CLI not available or not authenticated. Run: gh auth login")
            return 1
        try:
            repository = record.get("repository") or resolve_github_repository_fn(
                resolve_github_actions_settings_fn(load_optional_config_fn())
            )
        except ValueError as exc:
            print_fn(f"Error: {exc}")
            return 1
        try:
            record = refresh_cloud_record_fn(record, repository, require_snapshot=True)
        except RuntimeError as exc:
            print_fn(f"Error: {exc}")
            return 1

    rendered_record = normalize_cloud_record_fn(record)
    rendered_record["cost_summary"] = estimate_cloud_record_cost_fn(rendered_record, config)
    print_fn(cloud_record_summary_fn(rendered_record, config))
    for line in cloud_status_detail_lines_fn(record):
        print_fn(line)
    print_namespace_usage_summary_fn(rendered_record)
    print_billing_period_summary_fn(
        estimate_billing_period_totals_fn(list_cloud_records_fn(limit=None), config)
    )
    for line in cloud_status_job_lines_fn(record):
        print_fn(line)
    return 0


__all__ = ["cmd_cloud_status"]
