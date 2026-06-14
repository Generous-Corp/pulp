"""Cloud history command orchestration."""
from __future__ import annotations

from collections.abc import Callable
import argparse


def cmd_cloud_history(
    args: argparse.Namespace,
    *,
    load_optional_config_fn: Callable[[], dict],
    filter_cloud_records_fn: Callable[..., list[dict]],
    list_cloud_records_fn: Callable[..., list[dict]],
    cloud_history_lines_fn: Callable[..., list[str]],
    cloud_record_summary_fn: Callable[[dict, dict | None], str],
    print_billing_period_summary_fn: Callable[..., None],
    estimate_billing_period_totals_fn: Callable[[list[dict], dict | None], dict],
    resolve_github_actions_settings_fn: Callable[[dict], dict],
    resolve_github_repository_fn: Callable[[dict], str],
    fetch_github_repo_actions_billing_summary_fn: Callable[[str, dict | None], dict],
    print_github_repo_billing_summary_fn: Callable[[dict], None],
    print_fn: Callable[[str], None] = print,
) -> int:
    config = load_optional_config_fn()
    records = filter_cloud_records_fn(
        list_cloud_records_fn(limit=None),
        workflow_key=getattr(args, "workflow", None),
        provider=getattr(args, "provider", None),
    )
    if not records:
        print_fn("No tracked cloud runs found.")
        return 0

    limit = max(1, int(getattr(args, "limit", 10)))
    for line in cloud_history_lines_fn(records, config, limit=limit, summary_fn=cloud_record_summary_fn):
        print_fn(line)

    print_fn("")
    print_billing_period_summary_fn(estimate_billing_period_totals_fn(records, config))
    if getattr(args, "provider", None) in (None, "github-hosted"):
        try:
            repository = resolve_github_repository_fn(resolve_github_actions_settings_fn(config))
        except ValueError as exc:
            print_github_repo_billing_summary_fn({"status": "unavailable", "reason": str(exc)})
        else:
            print_github_repo_billing_summary_fn(
                fetch_github_repo_actions_billing_summary_fn(repository, config)
            )
    return 0


__all__ = ["cmd_cloud_history"]
