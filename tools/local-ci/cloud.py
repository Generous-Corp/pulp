"""Cloud provider integration for local CI — GitHub Actions + Namespace.

Cloud billing / provider-metadata helpers, provider comparison/recommendation,
GitHub and Namespace helper facade seams, and the cmd_cloud_* subcommands live
here. Public symbols are re-exported into local_ci.py for the non-cloud commands
+ main() dispatch.

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
from cloud_reporting_commands import (  # noqa: E402  -- re-exported for in-file consumers
    cmd_cloud_compare as _cmd_cloud_compare,
    cmd_cloud_defaults as _cmd_cloud_defaults,
    cmd_cloud_history as _cmd_cloud_history,
    cmd_cloud_recommend as _cmd_cloud_recommend,
    cmd_cloud_status as _cmd_cloud_status,
    cmd_cloud_workflows as _cmd_cloud_workflows,
)
from cloud_github_billing import (  # noqa: E402  -- re-exported for in-file consumers
    fetch_github_repo_actions_billing_summary as _fetch_github_repo_actions_billing_summary,
)
from cloud_provider_metadata import (  # noqa: E402  -- re-exported for in-file consumers
    enrich_cloud_record_provider_metadata as _enrich_cloud_record_provider_metadata,
)
from cloud_facade_helpers import (  # noqa: E402
    cloud_record_summary_with_deps as _cloud_record_summary_with_deps,
    enrich_cloud_record_provider_metadata_with_deps as _enrich_cloud_record_provider_metadata_with_deps,
    estimate_billing_period_totals_with_deps as _estimate_billing_period_totals_with_deps,
    fetch_github_repo_actions_billing_summary_with_deps as _fetch_github_repo_actions_billing_summary_with_deps,
    list_cloud_records_with_deps as _list_cloud_records_with_deps,
    namespace_instance_duration_secs_with_deps as _namespace_instance_duration_secs_with_deps,
    namespace_instances_for_run_with_deps as _namespace_instances_for_run_with_deps,
    normalize_namespace_instance_with_deps as _normalize_namespace_instance_with_deps,
    nsc_available_with_deps as _nsc_available_with_deps,
    nsc_instance_history_with_deps as _nsc_instance_history_with_deps,
    nsc_logged_in_with_deps as _nsc_logged_in_with_deps,
    nsc_version_with_deps as _nsc_version_with_deps,
    nsc_workspace_info_with_deps as _nsc_workspace_info_with_deps,
    refresh_cloud_record_with_deps as _refresh_cloud_record_with_deps,
    resolve_github_repository_with_deps as _resolve_github_repository_with_deps,
    save_cloud_record_with_deps as _save_cloud_record_with_deps,
    update_cloud_record_from_run_with_deps as _update_cloud_record_from_run_with_deps,
)
from cloud_facade_commands import (  # noqa: E402
    cmd_cloud_compare_with_deps as _cmd_cloud_compare_with_deps,
    cmd_cloud_defaults_with_deps as _cmd_cloud_defaults_with_deps,
    cmd_cloud_history_with_deps as _cmd_cloud_history_with_deps,
    cmd_cloud_namespace_doctor_with_deps as _cmd_cloud_namespace_doctor_with_deps,
    cmd_cloud_namespace_setup_with_deps as _cmd_cloud_namespace_setup_with_deps,
    cmd_cloud_recommend_with_deps as _cmd_cloud_recommend_with_deps,
    cmd_cloud_run_with_deps as _cmd_cloud_run_with_deps,
    cmd_cloud_status_with_deps as _cmd_cloud_status_with_deps,
    cmd_cloud_workflows_with_deps as _cmd_cloud_workflows_with_deps,
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
    return _save_cloud_record_with_deps(
        record,
        save_cloud_record_fn=_save_cloud_record,
        ensure_state_dirs_fn=ensure_state_dirs,
        cloud_run_path_fn=cloud_run_path,
        atomic_write_text_fn=atomic_write_text,
    )


def load_cloud_record(path: Path) -> dict:
    return _load_cloud_record(path)


def list_cloud_records(limit: int | None = None) -> list[dict]:
    return _list_cloud_records_with_deps(
        limit=limit,
        list_cloud_records_fn=_list_cloud_records,
        ensure_state_dirs_fn=ensure_state_dirs,
        cloud_runs_dir_fn=cloud_runs_dir,
        load_cloud_record_fn=load_cloud_record,
    )


def cloud_record_summary(record: dict, config: dict | None = None) -> str:
    return _cloud_record_summary_with_deps(
        record,
        config,
        cloud_record_summary_fn=_cloud_record_summary,
        estimate_cloud_record_cost_fn=estimate_cloud_record_cost,
        format_currency_amount_fn=format_currency_amount,
    )


def estimate_billing_period_totals(
    records: list[dict],
    config: dict | None,
    *,
    provider: str | None = None,
) -> dict:
    return _estimate_billing_period_totals_with_deps(
        records,
        config,
        provider=provider,
        estimate_billing_period_totals_fn=_estimate_billing_period_totals,
        billing_period_window_fn=billing_period_window,
    )


def fetch_github_repo_actions_billing_summary(repository: str, config: dict | None) -> dict:
    return _fetch_github_repo_actions_billing_summary_with_deps(
        repository,
        config,
        fetch_github_repo_actions_billing_summary_fn=_fetch_github_repo_actions_billing_summary,
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
    return _namespace_instance_duration_secs_with_deps(
        instance,
        namespace_instance_duration_secs_fn=_namespace_instance_duration_secs,
        now_iso_fn=now_iso,
    )


def normalize_namespace_instance(instance: dict) -> dict:
    return _normalize_namespace_instance_with_deps(
        instance,
        normalize_namespace_instance_fn=_normalize_namespace_instance,
        now_iso_fn=now_iso,
    )


def enrich_cloud_record_provider_metadata(record: dict) -> dict:
    return _enrich_cloud_record_provider_metadata_with_deps(
        record,
        enrich_cloud_record_provider_metadata_fn=_enrich_cloud_record_provider_metadata,
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
    return _nsc_available_with_deps(nsc_available_fn=_nsc_available, nsc_run_fn=nsc_run)


def nsc_version() -> str | None:
    return _nsc_version_with_deps(nsc_version_fn=_nsc_version, nsc_run_fn=nsc_run)


def nsc_logged_in() -> bool:
    return _nsc_logged_in_with_deps(nsc_logged_in_fn=_nsc_logged_in, nsc_run_fn=nsc_run)


def nsc_workspace_info() -> dict[str, str] | None:
    return _nsc_workspace_info_with_deps(
        nsc_workspace_info_fn=_nsc_workspace_info,
        nsc_run_fn=nsc_run,
    )


def nsc_instance_history(max_entries: int = 100) -> list[dict]:
    return _nsc_instance_history_with_deps(
        max_entries,
        nsc_instance_history_fn=_nsc_instance_history,
        nsc_run_fn=nsc_run,
    )


def namespace_instances_for_run(repository: str, run_id: int) -> list[dict]:
    return _namespace_instances_for_run_with_deps(
        repository,
        run_id,
        namespace_instances_for_run_fn=_namespace_instances_for_run,
        nsc_instance_history_fn=nsc_instance_history,
        normalize_namespace_instance_fn=normalize_namespace_instance,
    )


def cmd_cloud_namespace_doctor(_args: argparse.Namespace) -> int:
    return _cmd_cloud_namespace_doctor_with_deps(_args, globals())


def cmd_cloud_namespace_setup(_args: argparse.Namespace) -> int:
    return _cmd_cloud_namespace_setup_with_deps(_args, globals())


def resolve_github_repository(settings: dict) -> str:
    return _resolve_github_repository_with_deps(settings, gh_repo_name_fn=gh_repo_name)


def gh_pr_head(pr_ref: str) -> tuple[int, str, str] | None:
    return _gh_pr_head(pr_ref, gh_pr_list_open_fn=gh_pr_list_open, print_fn=print)


# ── CLI Commands ─────────────────────────────────────────────────────────────


def update_cloud_record_from_run(record: dict, snapshot: dict, *, provider_resolved: str | None = None) -> dict:
    return _update_cloud_record_from_run_with_deps(
        record,
        snapshot,
        provider_resolved=provider_resolved,
        update_cloud_record_from_run_fn=_update_cloud_record_from_run,
        now_iso_fn=now_iso,
    )


def refresh_cloud_record(record: dict, repository: str, *, require_snapshot: bool = False) -> dict:
    return _refresh_cloud_record_with_deps(
        record,
        repository,
        require_snapshot=require_snapshot,
        normalize_cloud_record_fn=normalize_cloud_record,
        gh_run_view_fn=gh_run_view,
        update_cloud_record_from_run_fn=update_cloud_record_from_run,
        enrich_cloud_record_provider_metadata_fn=enrich_cloud_record_provider_metadata,
        save_cloud_record_fn=save_cloud_record,
    )


def cmd_cloud_history(args: argparse.Namespace) -> int:
    return _cmd_cloud_history_with_deps(args, globals())


def cmd_cloud_compare(args: argparse.Namespace) -> int:
    return _cmd_cloud_compare_with_deps(args, globals())


def cmd_cloud_recommend(args: argparse.Namespace) -> int:
    return _cmd_cloud_recommend_with_deps(args, globals())


def cmd_cloud_workflows(_args: argparse.Namespace) -> int:
    return _cmd_cloud_workflows_with_deps(_args, globals())


def cmd_cloud_defaults(_args: argparse.Namespace) -> int:
    return _cmd_cloud_defaults_with_deps(_args, globals())


def cmd_cloud_run(args: argparse.Namespace) -> int:
    return _cmd_cloud_run_with_deps(args, globals())


def cmd_cloud_status(args: argparse.Namespace) -> int:
    return _cmd_cloud_status_with_deps(args, globals())
