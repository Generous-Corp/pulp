"""Cloud compare and recommendation command orchestration."""
from __future__ import annotations

from collections.abc import Callable
import argparse


def cmd_cloud_compare(
    args: argparse.Namespace,
    *,
    load_optional_config_fn: Callable[[], dict],
    resolve_github_actions_settings_fn: Callable[[dict], dict],
    compare_cloud_providers_fn: Callable[..., list[dict]],
    list_cloud_records_fn: Callable[..., list[dict]],
    cloud_compare_summary_line_fn: Callable[[dict], str],
    print_billing_period_summary_fn: Callable[..., None],
    print_fn: Callable[[str], None] = print,
) -> int:
    config = load_optional_config_fn()
    workflow_key = args.workflow or resolve_github_actions_settings_fn(config).get("workflow", "build")
    summaries = compare_cloud_providers_fn(list_cloud_records_fn(limit=None), config, workflow_key=workflow_key)
    if not summaries:
        print_fn(f"No tracked cloud runs found for workflow '{workflow_key}'.")
        return 0

    print_fn(f"Cloud compare: workflow={workflow_key}\n")
    for summary in summaries:
        print_fn(cloud_compare_summary_line_fn(summary))
        print_billing_period_summary_fn(summary.get("period") or {}, indent="    ")
    print_fn("\n  note: estimated; verify provider pricing")
    return 0


def cmd_cloud_recommend(
    args: argparse.Namespace,
    *,
    load_optional_config_fn: Callable[[], dict],
    resolve_github_actions_settings_fn: Callable[[dict], dict],
    recommend_cloud_provider_fn: Callable[..., tuple[str, str]],
    list_cloud_records_fn: Callable[..., list[dict]],
    cloud_recommend_lines_fn: Callable[[str, str, str], list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    config = load_optional_config_fn()
    workflow_key = args.workflow or resolve_github_actions_settings_fn(config).get("workflow", "build")
    provider, reason = recommend_cloud_provider_fn(list_cloud_records_fn(limit=None), config, workflow_key=workflow_key)
    for line in cloud_recommend_lines_fn(workflow_key, provider, reason):
        print_fn(line)
    return 0


__all__ = ["cmd_cloud_compare", "cmd_cloud_recommend"]
