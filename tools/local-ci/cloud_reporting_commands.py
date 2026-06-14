"""Cloud reporting/defaults/status command orchestration."""
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


def cmd_cloud_workflows(
    _args: argparse.Namespace,
    *,
    builtin_github_workflows: dict,
    cloud_workflow_lines_fn: Callable[[dict], list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    for line in cloud_workflow_lines_fn(builtin_github_workflows):
        print_fn(line)
    return 0


def cmd_cloud_defaults(
    _args: argparse.Namespace,
    *,
    load_optional_config_fn: Callable[[], dict],
    github_actions_settings_for_display_fn: Callable[[dict], dict],
    resolve_github_actions_settings_fn: Callable[[dict], dict],
    resolve_github_repository_fn: Callable[[dict], str],
    gh_available_fn: Callable[[], bool],
    gh_repo_variables_fn: Callable[[str], dict[str, str]],
    cloud_defaults_lines_fn: Callable[..., list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    config = load_optional_config_fn()
    settings = github_actions_settings_for_display_fn(config)
    repository = ""
    repository_note = ""
    repository_variables: dict[str, str] = {}
    try:
        resolved_settings = resolve_github_actions_settings_fn(config)
        settings = resolved_settings
        repository = resolve_github_repository_fn(resolved_settings)
    except ValueError as exc:
        repository_note = str(exc)
        try:
            repository = resolve_github_repository_fn(settings)
        except ValueError:
            repository = ""
    else:
        if gh_available_fn():
            repository_variables = gh_repo_variables_fn(repository)
        else:
            repository_note = "gh CLI unavailable; repo-variable fallbacks not inspected"

    for line in cloud_defaults_lines_fn(
        config,
        settings,
        repository=repository,
        repository_note=repository_note,
        repository_variables=repository_variables,
    ):
        print_fn(line)
    return 0


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
