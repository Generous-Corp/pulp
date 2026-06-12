"""Cloud provider integration for local CI — GitHub Actions + Namespace.

Extracted from local_ci.py (R2-1, #2645): cloud billing /
provider-metadata helpers, provider comparison/recommendation, GitHub and
Namespace helper facade seams, and the cmd_cloud_* subcommands. Public symbols
are re-exported into local_ci.py for the non-cloud commands + main() dispatch.

load_optional_config is still reached through the local_ci.py facade for
compatibility, but its default implementation is installed by
config_evidence_bindings.py. Import it lazily here (_load_optional_config) to
avoid an import cycle.
"""
from __future__ import annotations

import argparse
import base64
import os
import re
import shlex
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict
from pathlib import Path

# Repo root — same derivation as local_ci.py (both live in tools/local-ci/).
ROOT = Path(__file__).resolve().parents[2]

from state_paths import (  # noqa: E402  -- re-exported for in-file consumers
    state_dir,
    config_path,
    worktree_config_path,
    shared_config_path,
    queue_path,
    results_dir,
    cloud_runs_dir,
    evidence_path,
    logs_dir,
    bundles_dir,
    prepared_dir,
    desktop_state_dir,
    desktop_receipts_dir,
    queue_lock_path,
    evidence_lock_path,
    drain_lock_path,
    runner_info_path,
    ensure_state_dirs,
    job_logs_dir,
    target_log_path,
    prepare_target_log,
)
from footprint import (  # noqa: E402  -- re-exported for in-file consumers
    format_size_bytes,
    path_size_bytes,
    local_ci_state_footprint,
    describe_path_for_cleanup,
)
from io_utils import (  # noqa: E402  -- re-exported for in-file consumers
    LockBusyError,
    tail_lines,
    trim_line,
    atomic_write_text,
    image_change_summary,
    file_lock,
)
from git_helpers import (  # noqa: E402  -- re-exported for in-file consumers
    now_iso,
    current_branch,
    current_sha,
    git_root_for,
    resolve_git_ref_sha,
)
from normalize import (  # noqa: E402  -- re-exported for in-file consumers
    PRIORITY_VALUES,
    normalize_priority,
    priority_value,
    normalize_validation_mode,
    normalize_desktop_source_mode,
    default_desktop_artifact_root,
    normalize_publish_mode,
    parse_config_bool,
    normalize_desktop_optional_config,
    infer_desktop_adapter,
    default_desktop_bootstrap,
    default_desktop_capability_tier,
    normalize_desktop_config,
)
from github_workflows import (  # noqa: E402  -- re-exported for in-file consumers
    GITHUB_ACTIONS_DEFAULTS,
    BUILTIN_GITHUB_WORKFLOWS,
    REPO_VARIABLE_FALLBACKS,
    github_actions_settings_for_display,
    resolve_github_actions_settings,
    normalize_runs_on_json,
    resolve_workflow_runner_selector_json,
    resolve_workflow_dispatch_field_values,
    repo_variable_name_for_workflow_field,
    resolve_default_provider_for_workflow,
    resolve_workflow_field_value_and_source,
    resolve_workflow_dispatch_defaults,
    summarize_workflow_provider_defaults,
    resolve_cli_dispatch_field_values,
)
from job_queue import (  # noqa: E402  -- re-exported for in-file consumers
    normalize_job,
    load_queue_unlocked,
    save_queue_unlocked,
)
from targets import (  # noqa: E402  -- re-exported for in-file consumers
    enabled_targets,
    parse_targets_arg,
    resolve_targets,
)
from cloud_records import (  # noqa: E402  -- re-exported for in-file consumers
    cloud_record_sort_key,
    duration_between,
    find_cloud_record,
    format_duration_secs,
    format_memory_megabytes,
    normalize_cloud_record,
    normalize_github_timestamp,
    parse_iso_datetime,
    render_selector_value,
    summarize_runner_selector,
)
from cloud_record_store import (  # noqa: E402  -- re-exported for in-file consumers
    cloud_record_summary as _cloud_record_summary,
    cloud_run_path as _cloud_run_path,
    list_cloud_records as _list_cloud_records,
    load_cloud_record as _load_cloud_record,
    load_result as _load_result,
    save_cloud_record as _save_cloud_record,
)
from cloud_billing import (  # noqa: E402  -- re-exported for in-file consumers
    billing_note_text,
    billing_period_window,
    estimate_billing_period_totals as _estimate_billing_period_totals,
    estimate_cloud_record_cost,
    estimate_github_hosted_cost,
    estimate_namespace_cost,
    format_currency_amount,
    infer_job_os,
    iter_year_months,
    match_namespace_shape_rate,
    parse_iso_date,
    parse_optional_bool,
    parse_rate_value,
    print_billing_period_summary,
    print_github_repo_billing_summary,
    provider_billing_note_text,
    resolve_billing_settings,
)
from cloud_namespace_usage import (  # noqa: E402  -- re-exported for in-file consumers
    namespace_instance_duration_secs as _namespace_instance_duration_secs,
    normalize_namespace_instance as _normalize_namespace_instance,
    print_namespace_usage_summary,
    summarize_namespace_usage,
)
from cloud_namespace import (  # noqa: E402  -- re-exported for in-file consumers
    cmd_cloud_namespace_doctor as _cmd_cloud_namespace_doctor,
    cmd_cloud_namespace_setup as _cmd_cloud_namespace_setup,
    namespace_instances_for_run as _namespace_instances_for_run,
    nsc_available as _nsc_available,
    nsc_instance_history as _nsc_instance_history,
    nsc_logged_in as _nsc_logged_in,
    nsc_run as _nsc_run,
    nsc_version as _nsc_version,
    nsc_workspace_info as _nsc_workspace_info,
    parse_colon_separated_fields,
    print_namespace_setup_help,
)
from cloud_run_snapshot import (  # noqa: E402  -- re-exported for in-file consumers
    summarize_cloud_timing,
    update_cloud_record_from_run as _update_cloud_record_from_run,
)
from cloud_run_prepare import (  # noqa: E402  -- re-exported for in-file consumers
    cloud_run_record_payload,
    cloud_workflow_dispatch_fields,
)
from cloud_run_command import cmd_cloud_run as _cmd_cloud_run  # noqa: E402  -- re-exported for in-file consumers
from cloud_github_billing import (  # noqa: E402  -- re-exported for in-file consumers
    fetch_github_repo_actions_billing_summary as _fetch_github_repo_actions_billing_summary,
)
from cloud_provider_metadata import (  # noqa: E402  -- re-exported for in-file consumers
    enrich_cloud_record_provider_metadata as _enrich_cloud_record_provider_metadata,
)
from cloud_compare import (  # noqa: E402  -- re-exported for in-file consumers
    compare_cloud_providers,
    filter_cloud_records,
    median_or_none,
    recommend_cloud_provider,
)
from cloud_compare_format import cloud_compare_summary_line  # noqa: E402  -- re-exported for in-file consumers
from cloud_command_format import (  # noqa: E402  -- re-exported for in-file consumers
    cloud_dispatch_lines,
    cloud_final_status_line,
    cloud_history_lines,
    cloud_recent_status_lines,
    cloud_recommend_lines,
    cloud_workflow_lines,
)
from cloud_pr_format import (  # noqa: E402  -- re-exported for in-file consumers
    format_ci_comment,
    no_open_prs_line,
    open_pr_list_entry_lines,
    open_pr_list_lines,
    open_prs_header_line,
)
from cloud_status_format import (  # noqa: E402  -- re-exported for in-file consumers
    cloud_status_detail_lines,
    cloud_status_job_lines,
)
from cloud_defaults_format import (  # noqa: E402  -- re-exported for in-file consumers
    cloud_defaults_lines,
    cloud_field_detail_line,
)
from cloud_github import (  # noqa: E402  -- re-exported for in-file consumers
    gh_api_json,
    gh_auth_status_text,
    gh_available,
    gh_current_login,
    gh_find_dispatched_run,
    gh_pr_comment,
    gh_pr_create,
    gh_pr_head as _gh_pr_head,
    gh_pr_list_open,
    gh_pr_merge,
    gh_repo_name,
    gh_repo_variables,
    gh_run_view,
    gh_token_scopes as _gh_token_scopes,
    gh_workflow_dispatch,
)

def _load_optional_config():
    # Lazy import: local_ci imports this module at top level, so importing
    # local_ci at module scope here would cycle. The facade export is installed
    # by config_evidence_bindings.py.
    from local_ci import load_optional_config
    return load_optional_config()


def load_result(path: Path) -> dict:
    return _load_result(path)


def cloud_run_path(dispatch_id: str) -> Path:
    return _cloud_run_path(dispatch_id)


def save_cloud_record(record: dict) -> Path:
    return _save_cloud_record(
        record,
        ensure_state_dirs_fn=ensure_state_dirs,
        cloud_run_path_fn=cloud_run_path,
        atomic_write_text_fn=atomic_write_text,
    )


def load_cloud_record(path: Path) -> dict:
    return _load_cloud_record(path)


def list_cloud_records(limit: int | None = None) -> list[dict]:
    return _list_cloud_records(
        limit=limit,
        ensure_state_dirs_fn=ensure_state_dirs,
        cloud_runs_dir_fn=cloud_runs_dir,
        load_cloud_record_fn=load_cloud_record,
    )


def cloud_record_summary(record: dict, config: dict | None = None) -> str:
    return _cloud_record_summary(
        record,
        config,
        estimate_cloud_record_cost_fn=estimate_cloud_record_cost,
        format_currency_amount_fn=format_currency_amount,
    )


def estimate_billing_period_totals(
    records: list[dict],
    config: dict | None,
    *,
    provider: str | None = None,
) -> dict:
    return _estimate_billing_period_totals(
        records,
        config,
        provider=provider,
        period_window_func=billing_period_window,
    )


def fetch_github_repo_actions_billing_summary(repository: str, config: dict | None) -> dict:
    return _fetch_github_repo_actions_billing_summary(
        repository,
        config,
        resolve_billing_settings_fn=resolve_billing_settings,
        gh_available_fn=gh_available,
        gh_api_json_fn=gh_api_json,
        billing_period_window_fn=billing_period_window,
        iter_year_months_fn=iter_year_months,
        gh_token_scopes_fn=gh_token_scopes,
        parse_iso_date_fn=parse_iso_date,
        provider_billing_note_text_fn=provider_billing_note_text,
    )


def print_cloud_field_detail(
    name: str,
    value: str,
    source: str = "",
    *,
    indent: str = "    ",
    unset_note: str = "",
) -> None:
    print(cloud_field_detail_line(name, value, source, indent=indent, unset_note=unset_note))


def namespace_instance_duration_secs(instance: dict) -> float | None:
    return _namespace_instance_duration_secs(instance, now_iso_fn=now_iso)


def normalize_namespace_instance(instance: dict) -> dict:
    return _normalize_namespace_instance(instance, now_iso_fn=now_iso)


def enrich_cloud_record_provider_metadata(record: dict) -> dict:
    return _enrich_cloud_record_provider_metadata(
        record,
        normalize_cloud_record_fn=normalize_cloud_record,
        nsc_logged_in_fn=nsc_logged_in,
        namespace_instances_for_run_fn=namespace_instances_for_run,
        summarize_namespace_usage_fn=summarize_namespace_usage,
    )



def gh_token_scopes() -> set[str]:
    return _gh_token_scopes(gh_auth_status_text_fn=gh_auth_status_text)


def nsc_run(args: list[str], *, capture_output: bool = True) -> subprocess.CompletedProcess | None:
    return _nsc_run(args, capture_output=capture_output)


def nsc_available() -> bool:
    return _nsc_available(nsc_run_fn=nsc_run)


def nsc_version() -> str | None:
    return _nsc_version(nsc_run_fn=nsc_run)


def nsc_logged_in() -> bool:
    return _nsc_logged_in(nsc_run_fn=nsc_run)


def nsc_workspace_info() -> dict[str, str] | None:
    return _nsc_workspace_info(nsc_run_fn=nsc_run)


def nsc_instance_history(max_entries: int = 100) -> list[dict]:
    return _nsc_instance_history(max_entries=max_entries, nsc_run_fn=nsc_run)


def namespace_instances_for_run(repository: str, run_id: int) -> list[dict]:
    return _namespace_instances_for_run(
        repository,
        run_id,
        nsc_instance_history_fn=nsc_instance_history,
        normalize_namespace_instance_fn=normalize_namespace_instance,
    )


def cmd_cloud_namespace_doctor(_args: argparse.Namespace) -> int:
    return _cmd_cloud_namespace_doctor(
        _args,
        nsc_version_fn=nsc_version,
        nsc_logged_in_fn=nsc_logged_in,
        nsc_workspace_info_fn=nsc_workspace_info,
        print_namespace_setup_help_fn=print_namespace_setup_help,
    )


def cmd_cloud_namespace_setup(_args: argparse.Namespace) -> int:
    return _cmd_cloud_namespace_setup(
        _args,
        nsc_available_fn=nsc_available,
        nsc_logged_in_fn=nsc_logged_in,
        nsc_run_fn=nsc_run,
        cmd_cloud_namespace_doctor_fn=cmd_cloud_namespace_doctor,
        print_namespace_setup_help_fn=print_namespace_setup_help,
    )


def resolve_github_repository(settings: dict) -> str:
    repository = settings.get("repository", "").strip()
    if repository:
        return repository
    discovered = gh_repo_name()
    if discovered:
        return discovered
    raise ValueError(
        "Could not determine GitHub repository. Set github_actions.repository in tools/local-ci/config.json "
        "or make sure `gh repo view` works in this checkout."
    )


def gh_pr_head(pr_ref: str) -> tuple[int, str, str] | None:
    return _gh_pr_head(pr_ref, gh_pr_list_open_fn=gh_pr_list_open, print_fn=print)


# ── CLI Commands ─────────────────────────────────────────────────────────────


def update_cloud_record_from_run(record: dict, snapshot: dict, *, provider_resolved: str | None = None) -> dict:
    return _update_cloud_record_from_run(
        record,
        snapshot,
        provider_resolved=provider_resolved,
        now_iso_fn=now_iso,
    )


def refresh_cloud_record(record: dict, repository: str, *, require_snapshot: bool = False) -> dict:
    run_id = record.get("run_id")
    if not run_id:
        return normalize_cloud_record(record)
    snapshot = gh_run_view(repository, int(run_id))
    if not snapshot:
        if require_snapshot:
            raise RuntimeError(f"Failed to refresh GitHub run {run_id} from {repository}.")
        return normalize_cloud_record(record)
    refreshed = enrich_cloud_record_provider_metadata(
        update_cloud_record_from_run(record, snapshot)
    )
    save_cloud_record(refreshed)
    return refreshed


def cmd_cloud_history(args: argparse.Namespace) -> int:
    config = _load_optional_config()
    records = filter_cloud_records(
        list_cloud_records(limit=None),
        workflow_key=getattr(args, "workflow", None),
        provider=getattr(args, "provider", None),
    )
    if not records:
        print("No tracked cloud runs found.")
        return 0

    limit = max(1, int(getattr(args, "limit", 10)))
    for line in cloud_history_lines(records, config, limit=limit, summary_fn=cloud_record_summary):
        print(line)

    print()
    print_billing_period_summary(estimate_billing_period_totals(records, config))
    if getattr(args, "provider", None) in (None, "github-hosted"):
        try:
            repository = resolve_github_repository(resolve_github_actions_settings(config))
        except ValueError as exc:
            print_github_repo_billing_summary({"status": "unavailable", "reason": str(exc)})
        else:
            print_github_repo_billing_summary(fetch_github_repo_actions_billing_summary(repository, config))
    return 0


def cmd_cloud_compare(args: argparse.Namespace) -> int:
    config = _load_optional_config()
    workflow_key = args.workflow or resolve_github_actions_settings(config).get("workflow", "build")
    summaries = compare_cloud_providers(list_cloud_records(limit=None), config, workflow_key=workflow_key)
    if not summaries:
        print(f"No tracked cloud runs found for workflow '{workflow_key}'.")
        return 0

    print(f"Cloud compare: workflow={workflow_key}\n")
    for summary in summaries:
        print(cloud_compare_summary_line(summary))
        print_billing_period_summary(summary.get("period") or {}, indent="    ")
    print("\n  note: estimated; verify provider pricing")
    return 0


def cmd_cloud_recommend(args: argparse.Namespace) -> int:
    config = _load_optional_config()
    workflow_key = args.workflow or resolve_github_actions_settings(config).get("workflow", "build")
    provider, reason = recommend_cloud_provider(list_cloud_records(limit=None), config, workflow_key=workflow_key)
    for line in cloud_recommend_lines(workflow_key, provider, reason):
        print(line)
    return 0


def cmd_cloud_workflows(_args: argparse.Namespace) -> int:
    for line in cloud_workflow_lines(BUILTIN_GITHUB_WORKFLOWS):
        print(line)
    return 0


def cmd_cloud_defaults(_args: argparse.Namespace) -> int:
    config = _load_optional_config()
    settings = github_actions_settings_for_display(config)
    repository = ""
    repository_note = ""
    repository_variables: dict[str, str] = {}
    try:
        resolved_settings = resolve_github_actions_settings(config)
        settings = resolved_settings
        repository = resolve_github_repository(resolved_settings)
    except ValueError as exc:
        repository_note = str(exc)
        try:
            repository = resolve_github_repository(settings)
        except ValueError:
            repository = ""
    else:
        if gh_available():
            repository_variables = gh_repo_variables(repository)
        else:
            repository_note = "gh CLI unavailable; repo-variable fallbacks not inspected"

    for line in cloud_defaults_lines(
        config,
        settings,
        repository=repository,
        repository_note=repository_note,
        repository_variables=repository_variables,
    ):
        print(line)
    return 0


def cmd_cloud_run(args: argparse.Namespace) -> int:
    return _cmd_cloud_run(
        args,
        gh_available_fn=gh_available,
        load_optional_config_fn=_load_optional_config,
        resolve_github_actions_settings_fn=resolve_github_actions_settings,
        resolve_github_repository_fn=resolve_github_repository,
        builtin_github_workflows=BUILTIN_GITHUB_WORKFLOWS,
        current_branch_fn=current_branch,
        resolve_default_provider_for_workflow_fn=resolve_default_provider_for_workflow,
        gh_repo_variables_fn=gh_repo_variables,
        resolve_workflow_dispatch_defaults_fn=resolve_workflow_dispatch_defaults,
        resolve_cli_dispatch_field_values_fn=resolve_cli_dispatch_field_values,
        normalize_runs_on_json_fn=normalize_runs_on_json,
        resolve_workflow_field_value_and_source_fn=resolve_workflow_field_value_and_source,
        now_iso_fn=now_iso,
        normalize_cloud_record_fn=normalize_cloud_record,
        cloud_run_record_payload_fn=cloud_run_record_payload,
        gh_current_login_fn=gh_current_login,
        save_cloud_record_fn=save_cloud_record,
        cloud_workflow_dispatch_fields_fn=cloud_workflow_dispatch_fields,
        gh_workflow_dispatch_fn=gh_workflow_dispatch,
        gh_find_dispatched_run_fn=gh_find_dispatched_run,
        enrich_cloud_record_provider_metadata_fn=enrich_cloud_record_provider_metadata,
        update_cloud_record_from_run_fn=update_cloud_record_from_run,
        cloud_dispatch_lines_fn=cloud_dispatch_lines,
        refresh_cloud_record_fn=refresh_cloud_record,
        cloud_final_status_line_fn=cloud_final_status_line,
    )


def cmd_cloud_status(args: argparse.Namespace) -> int:
    config = _load_optional_config()
    if args.identifier is None:
        records = list_cloud_records(limit=args.limit)
        if not records:
            print("No tracked cloud runs yet.")
            return 0
        for line in cloud_recent_status_lines(records, config, summary_fn=cloud_record_summary):
            print(line)
        print_billing_period_summary(estimate_billing_period_totals(list_cloud_records(limit=None), config))
        return 0

    try:
        record = find_cloud_record(list_cloud_records(), args.identifier)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    if record is None:
        print("No matching cloud runs found.")
        return 1

    if args.refresh:
        if not gh_available():
            print("Error: gh CLI not available or not authenticated. Run: gh auth login")
            return 1
        try:
            repository = record.get("repository") or resolve_github_repository(
                resolve_github_actions_settings(_load_optional_config())
            )
        except ValueError as exc:
            print(f"Error: {exc}")
            return 1
        try:
            record = refresh_cloud_record(record, repository, require_snapshot=True)
        except RuntimeError as exc:
            print(f"Error: {exc}")
            return 1

    rendered_record = normalize_cloud_record(record)
    rendered_record["cost_summary"] = estimate_cloud_record_cost(rendered_record, config)
    print(cloud_record_summary(rendered_record, config))
    for line in cloud_status_detail_lines(record):
        print(line)
    print_namespace_usage_summary(rendered_record)
    print_billing_period_summary(estimate_billing_period_totals(list_cloud_records(limit=None), config))
    for line in cloud_status_job_lines(record):
        print(line)
    return 0
