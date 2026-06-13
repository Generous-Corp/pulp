#!/usr/bin/env python3
"""Local CI runner for Pulp — validates queued jobs on Mac, Ubuntu, and Windows.

Usage:
    pulp ci-local run [branch]                # Queue and wait for completion
    pulp ci-local run [branch] --smoke        # Fast install/export preflight, no tests
    pulp ci-local ship [branch]               # PR -> queued CI -> merge on green
    pulp ci-local check <PR#|latest>          # Validate an existing PR
    pulp ci-local check <PR#|latest> --smoke  # Fast PR smoke preflight
    pulp ci-local cloud workflows             # List supported GitHub workflows/providers
    pulp ci-local cloud defaults              # Show effective cloud defaults
    pulp ci-local cloud run [workflow]        # Dispatch a GitHub workflow
    pulp ci-local cloud status [id|latest]    # Show tracked GitHub workflow state
    pulp ci-local cloud history               # Show recent tracked cloud run history
    pulp ci-local cloud compare [workflow]    # Compare observed cloud providers
    pulp ci-local cloud recommend [workflow]  # Suggest a cloud provider from recorded history
    pulp ci-local cloud namespace doctor      # Check Namespace CLI/login/workspace state
    pulp ci-local cloud namespace setup       # Thin Namespace setup wrapper (`nsc login`)
    pulp ci-local list                        # Show open PRs
    pulp ci-local status                      # Show queue, runner, and VM status
    pulp ci-local enqueue [branch]            # Queue for later drain
    pulp ci-local drain                       # Drain pending jobs if no runner is active
    pulp ci-local bump <job> <priority>       # Reprioritize a pending job

Queueing model:
    - CI state is machine-global, not per worktree.
    - Only one drain owner runs jobs at a time.
    - Jobs are ordered by priority, then FIFO within each priority.
    - Each job validates an exact git SHA.
    - SSH targets receive the queued SHA via a git bundle before validation.
"""

from __future__ import annotations

import argparse
from collections import deque
from collections.abc import Callable
import fcntl
import json
import os
import plistlib
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import threading
import time
import uuid
import urllib.error
import urllib.parse
import urllib.request
from contextlib import contextmanager
from datetime import date, datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT_DIR = Path(__file__).resolve().parent
# Make sibling helper modules importable even when local_ci.py is loaded via
# importlib.util.spec_from_file_location (the path the unit tests take).
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
WAIT_POLL_SECS = 3
KEEP_COMPLETED_JOBS = 25
_BUNDLE_BUILD_LOCK = threading.Lock()
_SSH_TRANSIENT_PATTERNS = (
    "Connection reset by peer",
    "kex_exchange_identification",
    "Connection closed by remote host",
    "Connection timed out during banner exchange",
    "ssh_exchange_identification",
)
WINDOWS_REQUIRED_REMOTE_TOOLS = {
    "git": {"winget_id": "Git.Git", "required": True},
}
WINDOWS_OPTIONAL_REMOTE_TOOLS = {
    "gh": {"winget_id": "GitHub.cli", "required": False},
}
WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME = "pulp-validate"


def is_transient_ssh_failure_detail(detail: str) -> bool:
    text = detail or ""
    return any(pattern in text for pattern in _SSH_TRANSIENT_PATTERNS)


def run_ssh_subprocess(
    args: list[str],
    *,
    input: str | None = None,
    timeout: int | None = None,
    retries: int = 3,
    retry_delay_secs: float = 2.0,
) -> subprocess.CompletedProcess[str]:
    attempt = 0
    while True:
        attempt += 1
        result = subprocess.run(
            args,
            input=input,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        detail = "\n".join(part for part in [result.stderr.strip(), result.stdout.strip()] if part)
        if result.returncode == 0 or attempt >= retries or not is_transient_ssh_failure_detail(detail):
            return result
        time.sleep(retry_delay_secs * attempt)


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
    state_footprint_lines,
    describe_path_for_cleanup,
)

import cleanup as _cleanup  # noqa: E402
import cleanup_cli as _cleanup_cli  # noqa: E402
import cli_dispatch as _cli_dispatch  # noqa: E402
import cli_dispatch_bindings as _cli_dispatch_bindings  # noqa: E402
import desktop_action_commands_cli as _desktop_action_commands_cli  # noqa: E402
import desktop_actions as _desktop_actions  # noqa: E402
import desktop_artifacts as _desktop_artifacts  # noqa: E402
import desktop_command_bindings as _desktop_command_bindings  # noqa: E402
import desktop_commands_cli as _desktop_commands_cli  # noqa: E402
import desktop_cli as _desktop_cli  # noqa: E402
import desktop_doctor as _desktop_doctor  # noqa: E402
import desktop_probe_bindings as _desktop_probe_bindings  # noqa: E402
import desktop_reporting_bindings as _desktop_reporting_bindings  # noqa: E402
import desktop_setup_commands_cli as _desktop_setup_commands_cli  # noqa: E402
import evidence_cli as _evidence_cli  # noqa: E402
import git_helpers as _git_helpers  # noqa: E402
import io_utils as _io_utils  # noqa: E402
import linux_desktop_action as _linux_desktop_action  # noqa: E402
import linux_desktop_bindings as _linux_desktop_bindings  # noqa: E402
import linux_target as _linux_target  # noqa: E402
import local_ci_command_bindings as _local_ci_command_bindings  # noqa: E402
import local_ci_commands_cli as _local_ci_commands_cli  # noqa: E402
import logs_cli as _logs_cli  # noqa: E402
import macos_desktop as _macos_desktop  # noqa: E402
import macos_desktop_action as _macos_desktop_action  # noqa: E402
import macos_desktop_bindings as _macos_desktop_bindings  # noqa: E402
import video_artifacts as _video_artifacts  # noqa: E402
import queue_commands_cli as _queue_commands_cli  # noqa: E402
import queue_bindings as _queue_bindings  # noqa: E402
import queue_lifecycle as _queue_lifecycle  # noqa: E402
import queue_orchestrator as _queue_orchestrator  # noqa: E402
import execution as _execution  # noqa: E402
import execution_bindings as _execution_bindings  # noqa: E402

HEARTBEAT_INTERVAL_SECS = _execution.HEARTBEAT_INTERVAL_SECS
STUCK_IDLE_SECS = _execution.STUCK_IDLE_SECS
import reporting as _reporting  # noqa: E402
import runner_state as _runner_state  # noqa: E402
import source_prep as _source_prep  # noqa: E402
import source_prep_bindings as _source_prep_bindings  # noqa: E402
import ssh_bundle as _ssh_bundle  # noqa: E402
import target_preflight as _target_preflight  # noqa: E402
import target_preflight_bindings as _target_preflight_bindings  # noqa: E402
import utility_command_bindings as _utility_command_bindings  # noqa: E402
import windows_desktop_action as _windows_desktop_action  # noqa: E402
import windows_desktop_bindings as _windows_desktop_bindings  # noqa: E402
import windows_probe as _windows_probe  # noqa: E402
import windows_target as _windows_target  # noqa: E402

LINUX_REQUIRED_REMOTE_TOOLS = _linux_target.LINUX_REQUIRED_REMOTE_TOOLS
LINUX_OPTIONAL_REMOTE_TOOLS = _linux_target.LINUX_OPTIONAL_REMOTE_TOOLS


def bundle_ref_name(job_id: str) -> str:
    return _ssh_bundle.bundle_ref_name(job_id)


def remote_bundle_name(job_id: str) -> str:
    return _ssh_bundle.remote_bundle_name(job_id)


def create_job_bundle(job: dict) -> Path:
    return _ssh_bundle.create_job_bundle(
        job,
        ensure_state_dirs_fn=ensure_state_dirs,
        bundles_dir_fn=bundles_dir,
        bundle_build_lock=_BUNDLE_BUILD_LOCK,
        root=ROOT,
        run_fn=subprocess.run,
    )


def config_for_bundle_probe(job: dict, config: dict | None = None) -> dict:
    return _ssh_bundle.config_for_bundle_probe(
        job,
        config,
        load_config_file_fn=load_config_file,
        load_optional_config_fn=load_optional_config,
    )


def sync_job_bundle_to_ssh_host(host: str, job: dict, report_progress=None, config: dict | None = None) -> tuple[str, str]:
    return _ssh_bundle.sync_job_bundle_to_ssh_host(
        host,
        job,
        report_progress=report_progress,
        config=config,
        create_job_bundle_fn=create_job_bundle,
        remote_bundle_name_fn=remote_bundle_name,
        bundle_ref_name_fn=bundle_ref_name,
        config_for_bundle_probe_fn=config_for_bundle_probe,
        probe_uploaded_bundle_size_fn=probe_uploaded_bundle_size,
        now_iso_fn=now_iso,
        popen_fn=subprocess.Popen,
        stdout_pipe=subprocess.PIPE,
        stderr_pipe=subprocess.PIPE,
        timeout_expired_type=subprocess.TimeoutExpired,
        time_fn=time.time,
    )


def target_name_for_ssh_host(config: dict, host: str) -> str | None:
    for name, target_cfg in config.get("targets", {}).items():
        if name == host or target_cfg.get("host") == host:
            return name
    return None


def ssh_host_uses_windows_shell(config: dict, host: str) -> bool:
    target_name = target_name_for_ssh_host(config, host)
    if target_name:
        target_cfg = dict(config.get("targets", {}).get(target_name, {}))
        repo_path = str(target_cfg.get("repo_path") or "")
        if target_name.lower().startswith("win") or "\\" in repo_path:
            return True
    return host.lower().startswith("win")


def probe_uploaded_bundle_size(host: str, remote_name: str, *, config: dict) -> int | None:
    if ssh_host_uses_windows_shell(config, host):
        cmd = [
            "ssh",
            "-o",
            "BatchMode=yes",
            host,
            f"cmd /V:OFF /C if exist %USERPROFILE%\\{remote_name} for %I in (%USERPROFILE%\\{remote_name}) do @echo %~zI",
        ]
    else:
        cmd = [
            "ssh",
            "-o",
            "BatchMode=yes",
            host,
            f"sh -lc 'f=\"$HOME/{remote_name}\"; if [ -f \"$f\" ]; then wc -c < \"$f\"; fi'",
        ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
    if result.returncode != 0:
        return None
    output = result.stdout.strip().splitlines()
    if not output:
        return None
    value = output[-1].strip()
    try:
        return int(value)
    except ValueError:
        return None


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
    short_sha,
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

from cli_parser import build_local_ci_parser  # noqa: E402


def load_config() -> dict:
    path = config_path()
    return load_config_file(path)


def load_config_file(path: str | os.PathLike[str]) -> dict:
    path = Path(path)
    if not path.exists():
        raise FileNotFoundError(
            f"Local CI config not found at {path}. Copy tools/local-ci/config.example.json first."
        )
    return normalize_desktop_config(json.loads(path.read_text()))


def load_optional_config() -> dict | None:
    path = config_path()
    if not path.exists():
        return None
    return json.loads(path.read_text())


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


from provenance import (  # noqa: E402  -- re-exported for in-file consumers
    normalize_provenance,
    provenance_summary,
    normalize_result,
)


import evidence_index as evidence_index_module  # noqa: E402
from evidence_index import (  # noqa: E402  -- re-exported for tests and callers
    collect_evidence_groups_from_index,
    empty_evidence_index,
    evidence_entry_key,
    evidence_record_from_result,
    load_evidence_index_unlocked,
    merge_result_into_evidence_index,
    normalize_evidence_index,
    rebuild_evidence_index_unlocked,
    save_evidence_index_unlocked,
)


def load_evidence_index() -> dict:
    return evidence_index_module.load_evidence_index()


def update_evidence_index(result: dict, result_path: Path) -> None:
    evidence_index_module.update_evidence_index(result, result_path)


def collect_evidence_groups(branch: str | None = None, sha: str | None = None) -> dict[str, list[dict]]:
    return collect_evidence_groups_from_index(load_evidence_index(), branch=branch, sha=sha)


def print_evidence_summary(
    *,
    branch: str | None = None,
    sha: str | None = None,
    limit: int = 3,
    indent: str = "",
) -> bool:
    return evidence_index_module.print_evidence_summary_from_groups(
        collect_evidence_groups(branch=branch, sha=sha),
        limit=limit,
        indent=indent,
    )


def evidence_scope_header_line(branch: str | None, sha: str | None) -> str | None:
    return evidence_index_module.evidence_scope_header_line(branch, sha)


def evidence_empty_line(*, has_header: bool) -> str:
    return evidence_index_module.evidence_empty_line(has_header=has_header)


def save_config(config: dict) -> None:
    atomic_write_text(config_path(), json.dumps(config, indent=2) + "\n")


from job_queue import (  # noqa: E402  -- re-exported for in-file consumers
    normalize_job,
    load_queue_unlocked,
    save_queue_unlocked,
)


def load_queue() -> list[dict]:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        queue, changed = reconcile_running_jobs_unlocked(queue)
        if changed:
            save_queue_unlocked(queue)
        return queue


from targets import (  # noqa: E402  -- re-exported for in-file consumers
    enabled_targets,
    parse_targets_arg,
    resolve_targets,
)

from cloud import (  # noqa: E402  -- re-exported for in-file consumers (R2-1 #2645)
    billing_note_text,
    billing_period_window,
    cloud_record_sort_key,
    cloud_record_summary,
    cloud_run_path,
    cmd_cloud_compare,
    cmd_cloud_defaults,
    cmd_cloud_history,
    cmd_cloud_namespace_doctor,
    cmd_cloud_namespace_setup,
    cmd_cloud_recommend,
    cmd_cloud_run,
    cmd_cloud_status,
    cmd_cloud_workflows,
    compare_cloud_providers,
    duration_between,
    enrich_cloud_record_provider_metadata,
    estimate_billing_period_totals,
    estimate_cloud_record_cost,
    estimate_github_hosted_cost,
    estimate_namespace_cost,
    fetch_github_repo_actions_billing_summary,
    filter_cloud_records,
    find_cloud_record,
    format_ci_comment,
    format_currency_amount,
    format_duration_secs,
    format_memory_megabytes,
    gh_api_json,
    gh_auth_status_text,
    gh_available,
    gh_current_login,
    gh_find_dispatched_run,
    gh_pr_comment,
    gh_pr_create,
    gh_pr_head,
    gh_pr_list_open,
    gh_pr_merge,
    gh_repo_name,
    gh_repo_variables,
    gh_run_view,
    gh_token_scopes,
    gh_workflow_dispatch,
    infer_job_os,
    iter_year_months,
    list_cloud_records,
    load_cloud_record,
    load_result,
    match_namespace_shape_rate,
    median_or_none,
    namespace_instance_duration_secs,
    namespace_instances_for_run,
    normalize_cloud_record,
    normalize_github_timestamp,
    normalize_namespace_instance,
    nsc_available,
    nsc_instance_history,
    nsc_logged_in,
    nsc_run,
    nsc_version,
    nsc_workspace_info,
    parse_colon_separated_fields,
    parse_iso_date,
    parse_iso_datetime,
    parse_optional_bool,
    parse_rate_value,
    print_billing_period_summary,
    print_cloud_field_detail,
    print_github_repo_billing_summary,
    print_namespace_setup_help,
    print_namespace_usage_summary,
    provider_billing_note_text,
    recommend_cloud_provider,
    refresh_cloud_record,
    render_selector_value,
    resolve_billing_settings,
    resolve_github_repository,
    save_cloud_record,
    summarize_cloud_timing,
    summarize_namespace_usage,
    summarize_runner_selector,
    update_cloud_record_from_run,
    open_pr_list_lines,
)


def desktop_target_receipt_path(target_name: str) -> Path:
    return _desktop_artifacts.desktop_target_receipt_path(
        target_name,
        desktop_receipts_dir_fn=desktop_receipts_dir,
    )


def desktop_receipt_for(target_name: str) -> dict | None:
    return _desktop_artifacts.desktop_receipt_for(
        target_name,
        desktop_target_receipt_path_fn=desktop_target_receipt_path,
    )


def default_windows_session_task_name(target_name: str) -> str:
    return _windows_target.default_windows_session_task_name(target_name)


def desktop_target_contract(target_name: str, target: dict) -> dict:
    return _windows_target.desktop_target_contract(target_name, target)


def windows_path_join(*parts: str) -> str:
    return _windows_target.windows_path_join(*parts)


def windows_default_repo_checkout_path(home_dir: str | None) -> str:
    return _windows_target.windows_default_repo_checkout_path(home_dir)


def windows_repo_path_is_unsafe(repo_path: str | None, home_dir: str | None = None) -> bool:
    return _windows_target.windows_repo_path_is_unsafe(repo_path, home_dir)


def update_target_repo_path(config: dict, target_name: str, repo_path: str) -> None:
    return _windows_target.update_target_repo_path(config, target_name, repo_path)


def probe_windows_repo_checkout(host: str, repo_path: str | None) -> dict:
    return _desktop_probe_bindings.probe_windows_repo_checkout(globals(), host, repo_path)


def windows_repo_checkout_ready(probe: dict | None) -> bool:
    return _windows_target.windows_repo_checkout_ready(probe)


def ensure_windows_remote_repo_checkout(
    host: str,
    repo_path: str | None,
    *,
    remote_url: str | None = None,
    bundle_name: str | None = None,
    bundle_ref: str | None = None,
) -> dict:
    return _desktop_probe_bindings.ensure_windows_remote_repo_checkout(
        globals(),
        host,
        repo_path,
        remote_url=remote_url,
        bundle_name=bundle_name,
        bundle_ref=bundle_ref,
    )


def build_windows_session_agent_request(
    target_name: str,
    contract: dict,
    command: str,
    *,
    repo_path: str,
    action_name: str,
    label: str | None,
    pulp_app_automation: bool,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    settle_secs: float,
    timeout_secs: float,
) -> dict:
    return _windows_target.build_windows_session_agent_request(
        target_name,
        contract,
        command,
        repo_path=repo_path,
        action_name=action_name,
        label=label,
        pulp_app_automation=pulp_app_automation,
        capture_ui_snapshot=capture_ui_snapshot,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        capture_before=capture_before,
        settle_secs=settle_secs,
        timeout_secs=timeout_secs,
        default_desktop_label_fn=default_desktop_label,
    )


def resolve_desktop_target(config: dict, target_name: str) -> dict:
    desktop_targets = config.get("desktop_automation", {}).get("targets", {})
    if target_name not in desktop_targets:
        raise ValueError(f"Unknown desktop target '{target_name}'.")
    target = desktop_targets[target_name]
    if not target.get("enabled", True):
        raise ValueError(f"Desktop target '{target_name}' is disabled.")
    return target


def desktop_optional_capabilities(optional_cfg: dict | None) -> list[str]:
    return _desktop_doctor.desktop_optional_capabilities(optional_cfg)


def desktop_capabilities_for(adapter: str, tier: str, optional_cfg: dict | None = None) -> list[str]:
    return _desktop_doctor.desktop_capabilities_for(adapter, tier, optional_cfg)


def _desktop_check(name: str, ok: bool, detail: str, *, required: bool = True) -> dict:
    return _desktop_doctor.desktop_check(name, ok, detail, required=required)


def _check_writable_dir(path: Path) -> tuple[bool, str]:
    return _desktop_doctor.check_writable_dir(path)


def probe_windows_session_agent(host: str, contract: dict) -> dict:
    return _desktop_probe_bindings.probe_windows_session_agent(globals(), host, contract)


def probe_windows_remote_tooling(host: str) -> dict:
    return _desktop_probe_bindings.probe_windows_remote_tooling(globals(), host)


def install_windows_remote_tool(host: str, package_id: str, *, timeout: int = 900) -> None:
    return _desktop_probe_bindings.install_windows_remote_tool(globals(), host, package_id, timeout=timeout)


def ensure_windows_remote_tooling(host: str, *, install_optional: bool = False) -> dict:
    return _desktop_probe_bindings.ensure_windows_remote_tooling(globals(), host, install_optional=install_optional)


def windows_tooling_detail(probe: dict, tool_name: str, *, missing_hint: str | None = None) -> str:
    return _windows_target.windows_tooling_detail(probe, tool_name, missing_hint=missing_hint)


def windows_remote_tooling_ready(probe: dict) -> bool:
    return _windows_target.windows_remote_tooling_ready(probe, required_tools=WINDOWS_REQUIRED_REMOTE_TOOLS)


def desktop_doctor_checks(config: dict, target_name: str) -> list[dict]:
    return _desktop_probe_bindings.desktop_doctor_checks(globals(), config, target_name)


def webdriver_status_url(base_url: str) -> str:
    return _desktop_doctor.webdriver_status_url(base_url)


def probe_webdriver_endpoint(base_url: str, *, timeout: float = 5.0) -> dict:
    return _desktop_probe_bindings.probe_webdriver_endpoint(globals(), base_url, timeout=timeout)


def desktop_artifact_root(config: dict) -> Path:
    return _desktop_artifacts.desktop_artifact_root(config)


def windows_desktop_session_user(probe: dict | None) -> str:
    return _windows_target.windows_desktop_session_user(probe)


def windows_desktop_session_state(probe: dict | None) -> str:
    return _windows_target.windows_desktop_session_state(probe)


def windows_repo_checkout_detail(probe: dict | None, *, fallback_path: str | None = None) -> str:
    return _windows_target.windows_repo_checkout_detail(probe, fallback_path=fallback_path)


def create_desktop_run_bundle(config: dict, target_name: str, action: str) -> Path:
    return _desktop_artifacts.create_desktop_run_bundle(config, target_name, action)


def desktop_publish_root(config: dict) -> Path:
    return _desktop_artifacts.desktop_publish_root(config)


def create_desktop_publish_bundle(config: dict) -> Path:
    return _desktop_artifacts.create_desktop_publish_bundle(config)


def probe_linux_launch_backend(host: str) -> dict:
    return _linux_target.probe_linux_launch_backend(
        host,
        ssh_command_result_fn=ssh_command_result,
    )


def probe_linux_remote_tooling(host: str) -> dict:
    return _linux_target.probe_linux_remote_tooling(
        host,
        ssh_command_result_fn=ssh_command_result,
    )


def linux_tooling_detail(probe: dict, tool_name: str, *, missing_hint: str | None = None) -> str:
    return _linux_target.linux_tooling_detail(probe, tool_name, missing_hint=missing_hint)


def linux_remote_tooling_ready(probe: dict) -> bool:
    return _linux_target.linux_remote_tooling_ready(probe, required_tools=LINUX_REQUIRED_REMOTE_TOOLS)


def normalize_git_remote_for_http(remote_url: str | None) -> str | None:
    return _git_helpers.normalize_git_remote_for_http(remote_url)


def normalize_git_remote_for_clone(remote_url: str | None) -> str | None:
    return _git_helpers.normalize_git_remote_for_clone(remote_url)


def git_origin_http_url(repo_root: Path = ROOT) -> str | None:
    return _git_helpers.git_origin_http_url(repo_root, run_fn=subprocess.run)


def git_origin_clone_url(repo_root: Path = ROOT) -> str | None:
    return _git_helpers.git_origin_clone_url(repo_root, run_fn=subprocess.run)


def _clear_directory_contents(path: Path) -> None:
    return _reporting.clear_directory_contents(path)


def _copy_directory_contents(src: Path, dest: Path) -> None:
    return _reporting.copy_directory_contents(src, dest)


def _run_git(args: list[str], *, cwd: Path, check: bool = True) -> subprocess.CompletedProcess:
    return _git_helpers.run_git(args, cwd=cwd, check=check, run_fn=subprocess.run)


def publish_report_to_branch(config: dict, report: dict) -> dict:
    return _desktop_reporting_bindings.publish_report_to_branch(globals(), config, report)


def make_desktop_source_request(args: argparse.Namespace) -> dict:
    return _source_prep_bindings.make_desktop_source_request(globals(), args)


def desktop_source_cache_key(source_request: dict) -> str:
    return _source_prep_bindings.desktop_source_cache_key(globals(), source_request)


def desktop_source_root(target_name: str, source_request: dict) -> Path:
    return _source_prep_bindings.desktop_source_root(globals(), target_name, source_request)


def _command_path_rewrite_candidate(token: str) -> Path | None:
    return _source_prep_bindings.command_path_rewrite_candidate(globals(), token)


def _rewrite_launch_command_for_mapper(command: str | None, mapper, *, windows: bool = False) -> str | None:
    return _source_prep_bindings.rewrite_launch_command_for_mapper(globals(), command, mapper, windows=windows)


def _windows_command_join(parts: list[str]) -> str:
    return subprocess.list2cmdline(parts)


def rewrite_launch_command_for_source_root(command: str | None, source_root: Path) -> str | None:
    return _source_prep_bindings.rewrite_launch_command_for_source_root(globals(), command, source_root)


def rewrite_launch_command_for_posix_root(command: str | None, remote_root: str) -> str | None:
    return _source_prep_bindings.rewrite_launch_command_for_posix_root(globals(), command, remote_root)


def rewrite_launch_command_for_windows_root(command: str | None, remote_root: str) -> str | None:
    return _source_prep_bindings.rewrite_launch_command_for_windows_root(globals(), command, remote_root)


def split_windows_prepare_commands(command: str) -> list[str]:
    return _source_prep_bindings.split_windows_prepare_commands(globals(), command)


def validate_windows_prepare_commands(commands: list[str]) -> None:
    return _source_prep_bindings.validate_windows_prepare_commands(globals(), commands)


def attach_desktop_source_to_manifest(manifest: dict, source_context: dict | None) -> None:
    return _source_prep_bindings.attach_desktop_source_to_manifest(globals(), manifest, source_context)


def slugify_token(value: str, *, max_len: int = 48) -> str:
    return _reporting.slugify_token(value, max_len=max_len)


def stage_desktop_publish_report(
    config: dict,
    manifests: list[dict],
    *,
    output_dir: Path | None = None,
    label: str | None = None,
) -> dict:
    return _desktop_reporting_bindings.stage_desktop_publish_report(
        globals(),
        config,
        manifests,
        output_dir=output_dir,
        label=label,
    )


def desktop_publish_reports(config: dict, *, limit: int | None = None) -> list[dict]:
    return _desktop_reporting_bindings.desktop_publish_reports(globals(), config, limit=limit)


def write_desktop_publish_rollups(config: dict) -> None:
    return _desktop_reporting_bindings.write_desktop_publish_rollups(globals(), config)


def wait_for_path(path: Path, timeout_secs: float) -> Path:
    return _io_utils.wait_for_path(path, timeout_secs)


def count_view_tree_nodes(node: object) -> int:
    return _desktop_actions.count_view_tree_nodes(node)


def detect_macos_app_bundle(command: str | None) -> Path | None:
    return _macos_desktop.detect_macos_app_bundle(command)


def macos_bundle_id_for_app_path(app_path: Path) -> str | None:
    return _macos_desktop.macos_bundle_id_for_app_path(app_path)


def desktop_run_manifests(config: dict, *, target_name: str | None = None, action: str | None = None) -> list[dict]:
    return _desktop_reporting_bindings.desktop_run_manifests(
        globals(),
        config,
        target_name=target_name,
        action=action,
    )


def normalize_desktop_proof_source_mode(mode: str | None) -> str:
    return _reporting.normalize_desktop_proof_source_mode(mode)


def desktop_manifest_adapter(config: dict, manifest: dict) -> str:
    return _reporting.desktop_manifest_adapter(config, manifest)


def desktop_manifest_run_status(manifest: dict) -> str:
    return _reporting.desktop_manifest_run_status(manifest)


def desktop_manifest_source(manifest: dict) -> dict:
    return _reporting.desktop_manifest_source(manifest)


def desktop_proof_scope_for_adapter(adapter: str) -> str:
    return _reporting.desktop_proof_scope_for_adapter(adapter)


def desktop_run_summary(config: dict, manifest: dict) -> dict:
    return _reporting.desktop_run_summary(config, manifest)


def desktop_proof_summaries(
    config: dict,
    *,
    target_name: str | None = None,
    action: str | None = None,
    source_mode: str | None = None,
    sha: str | None = None,
    branch: str | None = None,
    limit: int | None = None,
) -> list[dict]:
    return _desktop_reporting_bindings.desktop_proof_summaries(
        globals(),
        config,
        target_name=target_name,
        action=action,
        source_mode=source_mode,
        sha=sha,
        branch=branch,
        limit=limit,
    )


def desktop_rollup_dir(config: dict, target_name: str | None = None) -> Path:
    return _desktop_reporting_bindings.desktop_rollup_dir(globals(), config, target_name)


def write_desktop_run_rollups(config: dict, *, target_name: str | None = None) -> None:
    return _desktop_reporting_bindings.write_desktop_run_rollups(globals(), config, target_name=target_name)


def prune_desktop_run_manifests(
    config: dict,
    *,
    target_name: str | None = None,
    older_than_days: int | None = None,
    keep_last: int | None = None,
) -> list[Path]:
    return _desktop_reporting_bindings.prune_desktop_run_manifests(
        globals(),
        config,
        target_name=target_name,
        older_than_days=older_than_days,
        keep_last=keep_last,
    )


def macos_window_probe_path() -> Path:
    return _macos_desktop.macos_window_probe_path(SCRIPT_DIR)


def macos_window_info_for_pid(pid: int) -> dict:
    return _macos_desktop.macos_window_info_for_pid(
        pid,
        probe_path_fn=macos_window_probe_path,
        run_fn=subprocess.run,
    )


def macos_window_info_for_bundle_id(bundle_id: str) -> dict:
    return _macos_desktop.macos_window_info_for_bundle_id(
        bundle_id,
        probe_path_fn=macos_window_probe_path,
        run_fn=subprocess.run,
    )


def macos_accessibility_trusted() -> bool:
    return _macos_desktop.macos_accessibility_trusted(
        probe_path_fn=macos_window_probe_path,
        run_fn=subprocess.run,
    )


def wait_for_macos_window(pid: int, timeout_secs: float) -> dict:
    return _macos_desktop.wait_for_macos_window(
        pid,
        timeout_secs,
        macos_window_info_for_pid_fn=macos_window_info_for_pid,
        time_fn=time.time,
        sleep_fn=time.sleep,
    )


def wait_for_macos_bundle_window(bundle_id: str, timeout_secs: float) -> tuple[int, dict]:
    return _macos_desktop.wait_for_macos_bundle_window(
        bundle_id,
        timeout_secs,
        macos_window_info_for_bundle_id_fn=macos_window_info_for_bundle_id,
        activate_macos_bundle_id_fn=activate_macos_bundle_id,
        time_fn=time.time,
        sleep_fn=time.sleep,
    )


def capture_macos_window(window_id: int, output_path: Path) -> None:
    _macos_desktop.capture_macos_window(
        window_id,
        output_path,
        run_fn=subprocess.run,
        sleep_fn=time.sleep,
    )


def probe_macos_screencapture() -> tuple[bool, str]:
    return _macos_desktop.probe_macos_screencapture(run_fn=subprocess.run)


def probe_macos_avfoundation_screen(ffmpeg_path: str | None = None) -> tuple[bool, str]:
    try:
        device = _macos_desktop.macos_avfoundation_screen_input_device(
            ffmpeg_path=ffmpeg_path or resolve_ffmpeg_path(),
            run_fn=subprocess.run,
        )
        return True, f"Capture screen 0 ({device})"
    except RuntimeError as exc:
        return False, str(exc)


def desktop_video_metadata(path: Path, **kwargs) -> dict:
    return _video_artifacts.desktop_video_metadata(path, **kwargs)


def write_desktop_video_metadata(path: Path, metadata: dict) -> None:
    _video_artifacts.write_desktop_video_metadata(path, metadata)


def compose_desktop_video_proof(
    manifest_path: Path,
    output_path: Path,
    *,
    template: str | None = None,
    source_image: Path | None = None,
    source_label: str | None = None,
    title: str | None = None,
) -> dict:
    return _video_artifacts.compose_desktop_video_proof(
        manifest_path,
        output_path,
        script_path=SCRIPT_DIR / "scripts" / "compose-video-proof.mjs",
        template=template,
        source_image=source_image,
        source_label=source_label,
        title=title,
        run_fn=subprocess.run,
    )


def video_proof_smoke() -> dict:
    command = ["npm", "--prefix", str(SCRIPT_DIR), "run", "smoke-video-proof"]
    try:
        result = subprocess.run(command, capture_output=True, text=True)
    except OSError as exc:
        return {"ok": False, "command": command, "detail": str(exc)}
    payload: dict = {}
    if result.stdout:
        json_start = result.stdout.find("{")
        try:
            payload = json.loads(result.stdout[json_start:]) if json_start >= 0 else {}
        except (ValueError, json.JSONDecodeError):
            payload = {}
    payload.update(
        {
            "ok": result.returncode == 0,
            "command": command,
            "returncode": result.returncode,
        }
    )
    if result.stdout:
        payload["stdout_tail"] = result.stdout[-4000:]
    if result.stderr:
        payload["stderr_tail"] = result.stderr[-4000:]
    if result.returncode == 0:
        payload["detail"] = payload.get("output") or "Remotion smoke render passed"
    else:
        payload["detail"] = (result.stderr or result.stdout or f"smoke exited {result.returncode}")[-1000:].strip()
    return payload


def create_issue_video_variant(
    source_path: Path,
    output_path: Path,
    metadata_path: Path,
    *,
    attachment_budget_bytes: int,
) -> dict:
    return _video_artifacts.create_issue_video_variant(
        source_path,
        output_path,
        metadata_path,
        attachment_budget_bytes=attachment_budget_bytes,
        ffmpeg_path=resolve_ffmpeg_path(),
        run_fn=subprocess.run,
    )


def resolve_ffmpeg_path() -> str:
    return _video_artifacts.resolve_ffmpeg_path(tool_dir=SCRIPT_DIR)


def start_macos_window_video_recording(
    window: dict,
    output_path: Path,
    *,
    duration_secs: float,
    fps: float,
) -> dict:
    return _macos_desktop.start_macos_window_video_recording(
        window,
        output_path,
        duration_secs=duration_secs,
        fps=fps,
        popen_fn=subprocess.Popen,
        run_fn=subprocess.run,
        ffmpeg_path=resolve_ffmpeg_path(),
    )


def stop_macos_window_video_recording(
    recording: dict,
    *,
    output_path: Path,
    metadata_path: Path,
    poster_path: Path | None = None,
    duration_secs: float,
    fps: float,
    attachment_budget_bytes: int,
) -> dict:
    return _macos_desktop.stop_macos_window_video_recording(
        recording,
        output_path=output_path,
        metadata_path=metadata_path,
        poster_path=poster_path,
        duration_secs=duration_secs,
        fps=fps,
        attachment_budget_bytes=attachment_budget_bytes,
        desktop_video_metadata_fn=desktop_video_metadata,
        write_desktop_video_metadata_fn=write_desktop_video_metadata,
    )


def parse_coordinate_pair(value: str, *, flag_name: str) -> tuple[float, float]:
    return _desktop_actions.parse_coordinate_pair(value, flag_name=flag_name)


def iter_view_tree_nodes(node: object, *, offset_x: float = 0.0, offset_y: float = 0.0):
    yield from _desktop_actions.iter_view_tree_nodes(node, offset_x=offset_x, offset_y=offset_y)


def resolve_view_tree_click_point(
    view_tree: dict,
    *,
    view_id: str | None,
    view_type: str | None,
    view_text: str | None,
    view_label: str | None,
) -> tuple[float, float]:
    return _desktop_actions.resolve_view_tree_click_point(
        view_tree,
        view_id=view_id,
        view_type=view_type,
        view_text=view_text,
        view_label=view_label,
    )


def screen_point_for_content_point(window: dict, content_size: tuple[float, float], content_point: tuple[float, float]) -> tuple[float, float]:
    return _desktop_actions.screen_point_for_content_point(window, content_size, content_point)


def activate_macos_pid(pid: int) -> dict:
    return _macos_desktop.activate_macos_pid(
        pid,
        probe_path_fn=macos_window_probe_path,
        run_fn=subprocess.run,
    )


def activate_macos_bundle_id(bundle_id: str) -> dict:
    return _macos_desktop.activate_macos_bundle_id(
        bundle_id,
        run_fn=subprocess.run,
    )


def dispatch_macos_click(screen_x: float, screen_y: float) -> dict:
    return _macos_desktop.dispatch_macos_click(
        screen_x,
        screen_y,
        probe_path_fn=macos_window_probe_path,
        run_fn=subprocess.run,
    )


def terminate_process(proc: subprocess.Popen, timeout_secs: float = 5.0) -> None:
    _macos_desktop.terminate_process(proc, timeout_secs=timeout_secs)


def quit_macos_bundle_id(bundle_id: str) -> None:
    _macos_desktop.quit_macos_bundle_id(
        bundle_id,
        run_fn=subprocess.run,
    )


def _local_worktree_matches(path: Path, sha: str) -> bool:
    return _source_prep_bindings.local_worktree_matches(globals(), path, sha)


def _reset_local_worktree(path: Path) -> None:
    return _source_prep_bindings.reset_local_worktree(globals(), path)


def prepare_macos_exact_sha_source(
    bundle_dir: Path,
    target_name: str,
    command: str,
    source_request: dict,
) -> dict:
    return _source_prep_bindings.prepare_macos_exact_sha_source(
        globals(),
        bundle_dir,
        target_name,
        command,
        source_request,
    )


def prepare_linux_exact_sha_source(
    bundle_dir: Path,
    target_name: str,
    host: str,
    command: str,
    source_request: dict,
) -> dict:
    return _source_prep_bindings.prepare_linux_exact_sha_source(
        globals(),
        bundle_dir,
        target_name,
        host,
        command,
        source_request,
    )


def prepare_windows_exact_sha_source(
    bundle_dir: Path,
    target_name: str,
    host: str,
    command: str,
    source_request: dict,
) -> dict:
    return _source_prep_bindings.prepare_windows_exact_sha_source(
        globals(),
        bundle_dir,
        target_name,
        host,
        command,
        source_request,
    )


def run_macos_local_smoke(
    config: dict,
    command: str | None,
    *,
    action_name: str = "smoke",
    bundle_id: str | None,
    label: str | None,
    output_path: str | None,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    pulp_app_automation: bool = False,
    capture_before: bool,
    settle_secs: float,
    timeout_secs: float,
    source_request: dict | None = None,
    record_video: bool = False,
    video_duration_secs: float = 8.0,
    video_fps: float = 30.0,
    video_attachment_budget_bytes: int = _video_artifacts.DEFAULT_VIDEO_ATTACHMENT_BUDGET_BYTES,
    compose_video_proof: bool = False,
) -> dict:
    return _macos_desktop_bindings.run_macos_local_smoke(
        globals(),
        config,
        command,
        action_name=action_name,
        bundle_id=bundle_id,
        label=label,
        output_path=output_path,
        capture_ui_snapshot=capture_ui_snapshot,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        pulp_app_automation=pulp_app_automation,
        capture_before=capture_before,
        settle_secs=settle_secs,
        timeout_secs=timeout_secs,
        source_request=source_request,
        record_video=record_video,
        video_duration_secs=video_duration_secs,
        video_fps=video_fps,
        video_attachment_budget_bytes=video_attachment_budget_bytes,
        compose_video_proof=compose_video_proof,
    )


def default_desktop_label(command: str | None, *, bundle_id: str | None = None) -> str:
    return _desktop_actions.default_desktop_label(command, bundle_id=bundle_id)


def remote_linux_bundle_relpath(target_name: str, action_name: str, bundle_dir: Path) -> str:
    return _linux_target.remote_linux_bundle_relpath(target_name, action_name, bundle_dir)


def fetch_ssh_artifact(host: str, remote_path: str, local_path: Path, *, optional: bool = False, timeout: int = 60) -> bool:
    return _linux_desktop_action.fetch_ssh_artifact(
        host,
        remote_path,
        local_path,
        optional=optional,
        timeout=timeout,
        run_fn=subprocess.run,
    )


def cleanup_remote_ssh_dir(host: str, remote_dir_expr: str) -> None:
    return _linux_desktop_action.cleanup_remote_ssh_dir(
        host,
        remote_dir_expr,
        ssh_command_result_fn=ssh_command_result,
    )


def build_linux_xvfb_remote_command(
    repo_path: str,
    remote_bundle_relpath: str,
    command: str,
    *,
    launch_backend: dict | None = None,
    launch_cwd: str | None = None,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    settle_secs: float,
) -> str:
    return _linux_target.build_linux_xvfb_remote_command(
        repo_path,
        remote_bundle_relpath,
        command,
        launch_backend=launch_backend,
        launch_cwd=launch_cwd,
        capture_ui_snapshot=capture_ui_snapshot,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        capture_before=capture_before,
        settle_secs=settle_secs,
    )


def build_linux_window_driver_remote_command(
    repo_path: str,
    remote_bundle_relpath: str,
    command: str,
    *,
    launch_backend: dict | None = None,
    launch_cwd: str | None = None,
    click_point: str | None,
    capture_before: bool,
    settle_secs: float,
) -> str:
    return _linux_target.build_linux_window_driver_remote_command(
        repo_path,
        remote_bundle_relpath,
        command,
        launch_backend=launch_backend,
        launch_cwd=launch_cwd,
        click_point=click_point,
        capture_before=capture_before,
        settle_secs=settle_secs,
        parse_coordinate_pair_fn=parse_coordinate_pair,
    )


def run_linux_xvfb_remote_action(
    config: dict,
    target_name: str,
    target: dict,
    command: str,
    *,
    action_name: str,
    label: str | None,
    output_path: str | None,
    pulp_app_automation: bool,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    settle_secs: float,
    timeout_secs: float,
    source_request: dict | None = None,
) -> dict:
    return _linux_desktop_bindings.run_linux_xvfb_remote_action(
        globals(),
        config,
        target_name,
        target,
        command,
        action_name=action_name,
        label=label,
        output_path=output_path,
        pulp_app_automation=pulp_app_automation,
        capture_ui_snapshot=capture_ui_snapshot,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        capture_before=capture_before,
        settle_secs=settle_secs,
        timeout_secs=timeout_secs,
        source_request=source_request,
    )


def run_windows_session_agent_action(
    config: dict,
    target_name: str,
    target: dict,
    command: str,
    *,
    action_name: str,
    label: str | None,
    output_path: str | None,
    pulp_app_automation: bool,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    settle_secs: float,
    timeout_secs: float,
    source_request: dict | None = None,
) -> dict:
    return _windows_desktop_bindings.run_windows_session_agent_action(
        globals(),
        config,
        target_name,
        target,
        command,
        action_name=action_name,
        label=label,
        output_path=output_path,
        pulp_app_automation=pulp_app_automation,
        capture_ui_snapshot=capture_ui_snapshot,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        capture_before=capture_before,
        settle_secs=settle_secs,
        timeout_secs=timeout_secs,
        source_request=source_request,
    )


def default_priority_for(command: str, config: dict) -> str:
    return _queue_bindings.default_priority_for(globals(), command, config)


def make_fingerprint(branch: str, sha: str, targets: list[str], validation: str) -> str:
    return _queue_bindings.make_fingerprint(globals(), branch, sha, targets, validation)


def make_job(
    branch: str,
    sha: str,
    priority: str,
    targets: list[str],
    mode: str,
    validation: str,
    submission: dict | None = None,
) -> dict:
    return _queue_bindings.make_job(
        globals(),
        branch,
        sha,
        priority,
        targets,
        mode,
        validation,
        submission=submission,
    )


def supersedence_key(job: dict) -> tuple[str, tuple[str, ...], str]:
    return _queue_bindings.supersedence_key(globals(), job)


def supersedence_identity_key(job: dict) -> tuple[str, str, str]:
    return _queue_bindings.supersedence_identity_key(globals(), job)


def jobs_share_supersedence_scope(newer_job: dict, older_job: dict) -> bool:
    return _queue_bindings.jobs_share_supersedence_scope(globals(), newer_job, older_job)


def job_has_narrower_same_identity_scope(newer_job: dict, older_job: dict) -> bool:
    return _queue_bindings.job_has_narrower_same_identity_scope(globals(), newer_job, older_job)


def supersedence_reason(newer_job: dict, older_job: dict) -> str | None:
    return _queue_bindings.supersedence_reason(globals(), newer_job, older_job)


def supersedence_result(job: dict, superseded_by: str, reason: str) -> dict:
    return _queue_bindings.supersedence_result(globals(), job, superseded_by, reason)


def supersede_job_unlocked(job: dict, superseded_by: str, reason: str) -> None:
    _queue_bindings.supersede_job_unlocked(globals(), job, superseded_by, reason)


def cancellation_result(job: dict, reason: str) -> dict:
    return _queue_bindings.cancellation_result(globals(), job, reason)


def cancel_job_unlocked(job: dict, reason: str = "operator_canceled") -> None:
    _queue_bindings.cancel_job_unlocked(globals(), job, reason)


def summarize_job(job: dict) -> str:
    return _queue_bindings.summarize_job(globals(), job)


def bump_queue_command_result_line(result: dict, job_ref: str) -> tuple[int, str]:
    return _queue_bindings.bump_queue_command_result_line(globals(), result, job_ref)


def cancel_queue_command_result_line(result: dict, job_ref: str) -> tuple[int, str]:
    return _queue_bindings.cancel_queue_command_result_line(globals(), result, job_ref)


def enqueue_command_result_line(job: dict, *, created: bool) -> str:
    return _queue_bindings.enqueue_command_result_line(globals(), job, created=created)


def drain_runner_active_line(runner_info: dict | None) -> str:
    return _queue_bindings.drain_runner_active_line(globals(), runner_info)


def summarize_active_targets(active_targets: dict | None, preferred_order: list[str] | None = None) -> str:
    return _queue_bindings.summarize_active_targets(globals(), active_targets, preferred_order)


def status_active_targets(job: dict, runner_info: dict | None = None) -> dict | None:
    return _queue_bindings.status_active_targets(globals(), job, runner_info)


def status_target_states(job: dict, active_targets: dict | None) -> list[tuple[str, dict]]:
    return _queue_bindings.status_target_states(globals(), job, active_targets)


def status_submission_lines(job: dict) -> list[str]:
    return _queue_bindings.status_submission_lines(globals(), job)


def target_state_detail_parts(state: dict) -> list[str]:
    return _queue_bindings.target_state_detail_parts(globals(), state)


def status_target_detail_lines(job: dict, active_targets: dict | None) -> list[str]:
    return _queue_bindings.status_target_detail_lines(globals(), job, active_targets)


def initial_target_state(job_id: str, target_name: str, *, started_at: str) -> dict:
    return _queue_bindings.initial_target_state(globals(), job_id, target_name, started_at=started_at)


def updated_target_state(previous_state: dict | None, fields: dict) -> dict:
    return _queue_bindings.updated_target_state(globals(), previous_state, fields)


def completed_target_state(
    job_id: str,
    target_name: str,
    result: dict,
    previous_state: dict | None,
    *,
    completed_at: str,
) -> dict:
    return _queue_bindings.completed_target_state(
        globals(),
        job_id,
        target_name,
        result,
        previous_state,
        completed_at=completed_at,
    )


def target_state_snapshot(target_states: dict[str, dict]) -> dict | None:
    return _queue_bindings.target_state_snapshot(globals(), target_states)


def status_runner_line(runner_info: dict | None) -> str:
    return _queue_bindings.status_runner_line(globals(), runner_info)


def recent_completed_status_line(job: dict, result: dict) -> str:
    return _queue_bindings.recent_completed_status_line(globals(), job, result)


def recent_completed_missing_result_line(job: dict) -> str:
    return _queue_bindings.recent_completed_missing_result_line(globals(), job)


def result_validation_line(result: dict) -> str | None:
    return _queue_bindings.result_validation_line(globals(), result)


def result_execution_line(result: dict) -> str:
    return _queue_bindings.result_execution_line(globals(), result)


def target_result_line(item: dict) -> str:
    return _queue_bindings.target_result_line(globals(), item)


def result_target_lines(result: dict) -> list[str]:
    return _queue_bindings.result_target_lines(globals(), result)


def result_overall_line(result: dict) -> str:
    return _queue_bindings.result_overall_line(globals(), result)


def missing_job_logs_line() -> str:
    return _queue_bindings.missing_job_logs_line(globals())


def missing_log_files_line(job: dict) -> str:
    return _queue_bindings.missing_log_files_line(globals(), job)


def job_logs_header_line(job: dict) -> str:
    return _queue_bindings.job_logs_header_line(globals(), job)


def log_section_header_line(target: str) -> str:
    return _queue_bindings.log_section_header_line(globals(), target)


def empty_log_line() -> str:
    return _queue_bindings.empty_log_line(globals())


def upsert_job_active_targets_unlocked(queue: list[dict], job_id: str, active_targets: dict | None) -> bool:
    return _queue_bindings.upsert_job_active_targets_unlocked(globals(), queue, job_id, active_targets)


def update_job_active_targets(job_id: str, active_targets: dict | None) -> None:
    _queue_bindings.update_job_active_targets(globals(), job_id, active_targets)


def enqueue_job(
    branch: str,
    sha: str,
    priority: str,
    targets: list[str],
    mode: str,
    validation: str,
    submission: dict | None = None,
) -> tuple[dict, bool]:
    return _queue_bindings.enqueue_job(
        globals(),
        branch,
        sha,
        priority,
        targets,
        mode,
        validation,
        submission=submission,
    )


def trim_completed_jobs_with_removed_ids(queue: list[dict]) -> tuple[list[dict], set[str]]:
    return _queue_bindings.trim_completed_jobs_with_removed_ids(globals(), queue)


def trim_completed_jobs(queue: list[dict]) -> list[dict]:
    return _queue_bindings.trim_completed_jobs(globals(), queue)


def bump_queue_command_job(job_ref: str, requested_priority: str) -> dict:
    return _queue_bindings.bump_queue_command_job(globals(), job_ref, requested_priority)


def cancel_queue_command_job(job_ref: str) -> dict:
    return _queue_bindings.cancel_queue_command_job(globals(), job_ref)


def result_file_job_id(path: Path) -> str | None:
    return _cleanup.result_file_job_id(path)


def artifact_entry_sort_key(entry: dict) -> tuple[float, str]:
    return _cleanup.artifact_entry_sort_key(entry)


def collect_local_ci_cleanup_plan(
    queue: list[dict],
    *,
    keep_results: int = KEEP_COMPLETED_JOBS,
    keep_logs: int = KEEP_COMPLETED_JOBS,
    keep_bundles: int = 0,
    include_prepared: bool = False,
) -> dict:
    return _cleanup.collect_local_ci_cleanup_plan(
        queue,
        keep_results=keep_results,
        keep_logs=keep_logs,
        keep_bundles=keep_bundles,
        include_prepared=include_prepared,
        bundles_dir_fn=bundles_dir,
        logs_dir_fn=logs_dir,
        results_dir_fn=results_dir,
        prepared_dir_fn=prepared_dir,
        path_size_bytes_fn=path_size_bytes,
    )


def apply_local_ci_cleanup_plan(plan: dict) -> dict:
    return _cleanup.apply_local_ci_cleanup_plan(plan)


def cleanup_plan_lines(plan: dict, *, dry_run: bool) -> list[str]:
    return _cleanup.cleanup_plan_lines(
        plan,
        dry_run=dry_run,
        format_size_fn=format_size_bytes,
        describe_path_fn=describe_path_for_cleanup,
    )


def job_sort_key(job: dict) -> tuple[int, str, str]:
    return _queue_bindings.job_sort_key(globals(), job)


def queue_status_groups(queue: list[dict]) -> tuple[list[dict], list[dict], list[dict]]:
    return _queue_bindings.queue_status_groups(globals(), queue)


def recent_completed_jobs_for_status(completed_jobs: list[dict], *, limit: int = 5) -> list[dict]:
    return _queue_bindings.recent_completed_jobs_for_status(globals(), completed_jobs, limit=limit)


def reconcile_running_jobs_unlocked(queue: list[dict]) -> tuple[list[dict], bool]:
    return _queue_bindings.reconcile_running_jobs_unlocked(globals(), queue)


def read_runner_info() -> dict | None:
    return _queue_bindings.read_runner_info(globals())


def pid_alive(pid: int | None) -> bool:
    return _queue_bindings.pid_alive(globals(), pid)


def current_runner_info() -> dict | None:
    return _queue_bindings.current_runner_info(globals())


def stale_running_jobs_unlocked(queue: list[dict]) -> list[dict]:
    return _queue_bindings.stale_running_jobs_unlocked(globals(), queue)


def update_job_target_state(job_id: str, target_name: str, **fields) -> None:
    _queue_bindings.update_job_target_state(globals(), job_id, target_name, **fields)


def collect_stale_windows_cleanup_candidates_unlocked(queue: list[dict]) -> list[dict]:
    return _cleanup.collect_stale_windows_cleanup_candidates_unlocked(
        queue,
        stale_running_jobs_fn=stale_running_jobs_unlocked,
        now_fn=now_iso,
    )


def cleanup_stale_windows_validator(host: str, pid: int, started_at: str) -> dict:
    return _cleanup.cleanup_stale_windows_validator(
        host,
        pid,
        started_at,
        ps_literal_fn=ps_literal,
        run_logged_command_fn=run_logged_command,
        windows_ssh_powershell_command_fn=windows_ssh_powershell_command,
        trim_line_fn=trim_line,
    )


def reclaim_stale_remote_validators(_config: dict) -> int:
    return _queue_bindings.reclaim_stale_remote_validators(globals(), _config)


def write_runner_info(info: dict) -> None:
    _queue_bindings.write_runner_info(globals(), info)


def update_runner_active_targets(job_id: str, active_targets: dict | None) -> None:
    _queue_bindings.update_runner_active_targets(globals(), job_id, active_targets)


def clear_runner_info() -> None:
    _queue_bindings.clear_runner_info(globals())


def find_job_unlocked(queue: list[dict], job_ref: str, statuses: set[str] | None = None) -> dict | None:
    return _queue_bindings.find_job_unlocked(globals(), queue, job_ref, statuses)


def load_job(job_id: str) -> dict | None:
    return _queue_bindings.load_job(globals(), job_id)


def claim_next_job() -> dict | None:
    return _queue_bindings.claim_next_job(globals())


def finalize_job(job_id: str, result: dict, result_path: Path) -> None:
    _queue_bindings.finalize_job(globals(), job_id, result, result_path)


def wait_for_job(job_id: str, config: dict) -> tuple[dict | None, int]:
    return _queue_bindings.wait_for_job(globals(), job_id, config)


def notify(message: str) -> None:
    print("\a", end="", flush=True)
    try:
        subprocess.run(
            ["osascript", "-e", f'display notification "{message}" with title "Pulp CI"'],
            capture_output=True,
            timeout=5,
        )
    except Exception:
        pass


# ── VM Management ────────────────────────────────────────────────────────────


def ssh_probe(host: str, timeout: int = 5) -> subprocess.CompletedProcess[str]:
    return _target_preflight_bindings.ssh_probe(globals(), host, timeout)


def ssh_reachable(host: str, timeout: int = 5) -> bool:
    return _target_preflight_bindings.ssh_reachable(globals(), host, timeout)


def ssh_failure_detail(host: str, timeout: int = 5) -> str:
    return _target_preflight_bindings.ssh_failure_detail(globals(), host, timeout)


def ssh_command_result(host: str, remote_cmd: str, *, timeout: int = 30) -> subprocess.CompletedProcess[str]:
    return _target_preflight_bindings.ssh_command_result(globals(), host, remote_cmd, timeout=timeout)


def utmctl_vm_status(vm_name: str) -> str | None:
    return _target_preflight_bindings.utmctl_vm_status(globals(), vm_name)


def utmctl_start(vm_name: str) -> bool:
    return _target_preflight_bindings.utmctl_start(globals(), vm_name)


def ensure_host_reachable(target_name: str, target_cfg: dict, defaults: dict) -> str | None:
    return _target_preflight_bindings.ensure_host_reachable(globals(), target_name, target_cfg, defaults)


def config_source_name(path: Path) -> str:
    return _target_preflight_bindings.config_source_name(globals(), path)


def config_material_for_targets(config: dict, targets: list[str]) -> dict:
    return _target_preflight_bindings.config_material_for_targets(globals(), config, targets)


def find_material_config_drift(targets: list[str]) -> list[str]:
    return _target_preflight_bindings.find_material_config_drift(globals(), targets)


def preflight_target_host_state(target_name: str, target_cfg: dict, defaults: dict) -> dict:
    return _target_preflight_bindings.preflight_target_host_state(globals(), target_name, target_cfg, defaults)


def build_submission_metadata(
    config: dict,
    branch: str,
    sha: str,
    targets: list[str],
    priority: str,
    validation: str,
    *,
    allow_root_mismatch: bool,
    allow_unreachable_targets: bool,
) -> dict:
    return _target_preflight_bindings.build_submission_metadata(
        globals(),
        config,
        branch,
        sha,
        targets,
        priority,
        validation,
        allow_root_mismatch=allow_root_mismatch,
        allow_unreachable_targets=allow_unreachable_targets,
    )


def print_submission_metadata(metadata: dict) -> None:
    return _target_preflight_bindings.print_submission_metadata(globals(), metadata)


# ── Validation Runners ───────────────────────────────────────────────────────


def remote_commit_error(target_name: str, host: str, job: dict) -> str:
    return _execution_bindings.remote_commit_error(globals(), target_name, host, job)


def parse_progress_marker(line: str) -> dict:
    return _execution_bindings.parse_progress_marker(globals(), line)


def prepared_state_root(target_name: str, validation: str) -> Path:
    return _execution_bindings.prepared_state_root(globals(), target_name, validation)


def should_reuse_prepared_state(job: dict) -> bool:
    return _execution_bindings.should_reuse_prepared_state(globals(), job)


def local_validation_command(job: dict, exclude_tests: str = "") -> tuple[list[str], str]:
    return _execution_bindings.local_validation_command(globals(), job, exclude_tests)


def posix_ssh_validation_command(
    target_name: str,
    host: str,
    repo_path: str,
    job: dict,
    *,
    bundle_name: str,
    bundle_ref: str,
    exclude_tests: str = "",
) -> tuple[list[str], str]:
    return _execution_bindings.posix_ssh_validation_command(
        globals(),
        target_name,
        host,
        repo_path,
        job,
        bundle_name=bundle_name,
        bundle_ref=bundle_ref,
        exclude_tests=exclude_tests,
    )


def validation_result_from_run(
    target_name: str,
    run: dict,
    *,
    log_path: Path,
    validation: str,
    transport_mode: str,
    timeout_secs: int = 3600,
) -> dict:
    return _execution_bindings.validation_result_from_run(
        globals(),
        target_name,
        run,
        log_path=log_path,
        validation=validation,
        transport_mode=transport_mode,
        timeout_secs=timeout_secs,
    )


def validation_error_result(
    target_name: str,
    detail: str,
    *,
    log_path: Path,
    transport_mode: str,
) -> dict:
    return _execution_bindings.validation_error_result(
        globals(),
        target_name,
        detail,
        log_path=log_path,
        transport_mode=transport_mode,
    )


def unreachable_target_result(target_name: str, detail: str = "Host unreachable") -> dict:
    return _execution_bindings.unreachable_target_result(globals(), target_name, detail)


def target_exception_result(target_name: str, exc: Exception) -> dict:
    return _execution_bindings.target_exception_result(globals(), target_name, exc)


def completed_job_result(job: dict, results: list[dict]) -> dict:
    return _execution_bindings.completed_job_result(globals(), job, results)


def sorted_target_results(results: list[dict]) -> list[dict]:
    return _execution_bindings.sorted_target_results(globals(), results)


def run_target_tasks(
    tasks: list[tuple[str, Callable[[], dict]]],
    *,
    on_target_complete: Callable[[str, dict], None],
) -> list[dict]:
    return _execution_bindings.run_target_tasks(globals(), tasks, on_target_complete=on_target_complete)


def run_logged_command(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    input_text: str | None = None,
    timeout: int = 3600,
    log_path: Path | None = None,
    report_progress=None,
    heartbeat_interval_secs: float = HEARTBEAT_INTERVAL_SECS,
    stuck_idle_secs: float = STUCK_IDLE_SECS,
) -> dict:
    return _execution_bindings.run_logged_command(
        globals(),
        cmd,
        cwd=cwd,
        input_text=input_text,
        timeout=timeout,
        log_path=log_path,
        report_progress=report_progress,
        heartbeat_interval_secs=heartbeat_interval_secs,
        stuck_idle_secs=stuck_idle_secs,
    )


def run_local_validation(job: dict, exclude_tests: str = "", report_progress=None) -> dict:
    return _execution_bindings.run_local_validation(
        globals(),
        job,
        exclude_tests,
        report_progress,
    )


def run_posix_ssh_validation(
    target_name: str,
    host: str,
    repo_path: str,
    job: dict,
    exclude_tests: str = "",
    config: dict | None = None,
    report_progress=None,
) -> dict:
    return _execution_bindings.run_posix_ssh_validation(
        globals(),
        target_name,
        host,
        repo_path,
        job,
        exclude_tests,
        config,
        report_progress,
    )


def ps_literal(value: str) -> str:
    return _windows_probe.ps_literal(value)


def windows_validation_script(
    target_name: str,
    host: str,
    effective_repo_path: str,
    job: dict,
    *,
    bundle_name: str,
    bundle_ref: str,
    exclude_tests: str,
    cmake_generator: str,
    resolved_platform: str,
    resolved_generator_instance: str,
) -> tuple[str, str]:
    return _execution_bindings.windows_validation_script(
        globals(),
        target_name,
        host,
        effective_repo_path,
        job,
        bundle_name=bundle_name,
        bundle_ref=bundle_ref,
        exclude_tests=exclude_tests,
        cmake_generator=cmake_generator,
        resolved_platform=resolved_platform,
        resolved_generator_instance=resolved_generator_instance,
    )


def validate_ci_branch_name(branch: str) -> str:
    return _queue_bindings.validate_ci_branch_name(globals(), branch)


def windows_ssh_powershell_command(host: str) -> list[str]:
    return _windows_probe.windows_ssh_powershell_command(host)


def run_windows_ssh_powershell(host: str, ps_script: str, *, timeout: int = 60) -> subprocess.CompletedProcess[str]:
    return _windows_probe.run_windows_ssh_powershell(
        host,
        ps_script,
        timeout=timeout,
        run_ssh_subprocess_fn=run_ssh_subprocess,
    )


def parse_windows_ssh_json(stdout: str) -> dict:
    return _windows_probe.parse_windows_ssh_json(stdout)


def windows_contract_expand_expression(raw_value: str) -> str:
    return _windows_probe.windows_contract_expand_expression(raw_value, ps_literal_fn=ps_literal)


def windows_session_agent_template_path() -> Path:
    return _windows_probe.windows_session_agent_template_path(SCRIPT_DIR)


def windows_ssh_write_text(host: str, remote_path: str, content: str) -> None:
    return _windows_probe.windows_ssh_write_text(
        host,
        remote_path,
        content,
        run_windows_ssh_powershell_fn=run_windows_ssh_powershell,
        parse_windows_ssh_json_fn=parse_windows_ssh_json,
        windows_contract_expand_expression_fn=windows_contract_expand_expression,
        ps_literal_fn=ps_literal,
    )


def windows_ssh_fetch_file(
    host: str,
    remote_path: str,
    local_path: Path,
    *,
    optional: bool = False,
    timeout: int = 60,
) -> bool:
    return _windows_probe.windows_ssh_fetch_file(
        host,
        remote_path,
        local_path,
        optional=optional,
        timeout=timeout,
        run_windows_ssh_powershell_fn=run_windows_ssh_powershell,
        windows_contract_expand_expression_fn=windows_contract_expand_expression,
    )


def windows_ssh_read_json(
    host: str,
    remote_path: str,
    *,
    timeout: int = 30,
    optional: bool = False,
) -> dict | None:
    return _windows_probe.windows_ssh_read_json(
        host,
        remote_path,
        timeout=timeout,
        optional=optional,
        run_windows_ssh_powershell_fn=run_windows_ssh_powershell,
        windows_contract_expand_expression_fn=windows_contract_expand_expression,
    )


def windows_ssh_remove_path(host: str, remote_path: str) -> None:
    return _windows_probe.windows_ssh_remove_path(
        host,
        remote_path,
        run_windows_ssh_powershell_fn=run_windows_ssh_powershell,
        windows_contract_expand_expression_fn=windows_contract_expand_expression,
    )


def bootstrap_windows_session_agent(host: str, contract: dict) -> dict:
    return _windows_probe.bootstrap_windows_session_agent(
        host,
        contract,
        windows_session_agent_template_path_fn=windows_session_agent_template_path,
        windows_ssh_write_text_fn=windows_ssh_write_text,
        run_windows_ssh_powershell_fn=run_windows_ssh_powershell,
        parse_windows_ssh_json_fn=parse_windows_ssh_json,
        windows_contract_expand_expression_fn=windows_contract_expand_expression,
        ps_literal_fn=ps_literal,
    )


def start_windows_session_agent_task(host: str, contract: dict) -> None:
    return _windows_probe.start_windows_session_agent_task(
        host,
        contract,
        run_windows_ssh_powershell_fn=run_windows_ssh_powershell,
        parse_windows_ssh_json_fn=parse_windows_ssh_json,
        ps_literal_fn=ps_literal,
    )


def probe_windows_ssh_cmake_settings(
    host: str,
    cmake_generator: str,
    cmake_platform: str,
    cmake_generator_instance: str,
) -> tuple[str, str]:
    return _windows_probe.probe_windows_ssh_cmake_settings(
        host,
        cmake_generator,
        cmake_platform,
        cmake_generator_instance,
        windows_ssh_powershell_command_fn=windows_ssh_powershell_command,
        run_fn=subprocess.run,
        ps_literal_fn=ps_literal,
    )


def run_windows_ssh_validation(
    target_name: str,
    host: str,
    repo_path: str,
    job: dict,
    exclude_tests: str = "",
    cmake_generator: str = "Visual Studio 17 2022",
    cmake_platform: str = "",
    cmake_generator_instance: str = "",
    config: dict | None = None,
    report_progress=None,
) -> dict:
    return _execution_bindings.run_windows_ssh_validation(
        globals(),
        target_name,
        host,
        repo_path,
        job,
        exclude_tests,
        cmake_generator,
        cmake_platform,
        cmake_generator_instance,
        config,
        report_progress,
    )


# ── Job Processing ───────────────────────────────────────────────────────────


def config_for_job_execution(job: dict, config: dict) -> dict:
    return _execution_bindings.config_for_job_execution(globals(), job, config)


def submission_target_state(job: dict, target_name: str) -> dict:
    return _execution_bindings.submission_target_state(globals(), job, target_name)


def resolve_ssh_target_execution(job: dict, target_name: str, target_cfg: dict, defaults: dict) -> tuple[str | None, str | None]:
    return _execution_bindings.resolve_ssh_target_execution(globals(), job, target_name, target_cfg, defaults)


def _build_target_tasks(job: dict, config: dict, progress_factory=None) -> list[tuple[str, Callable[[], dict]]]:
    return _execution_bindings.build_target_tasks(globals(), job, config, progress_factory)


def process_job(job: dict, config: dict) -> dict:
    return _execution_bindings.process_job(globals(), job, config)


def save_result(result: dict) -> Path:
    return _execution_bindings.save_result(globals(), result)


def print_result(result: dict, result_path: Path | None = None) -> None:
    return _execution_bindings.print_result(globals(), result, result_path)


def drain_pending_jobs(config: dict, *, blocking: bool) -> tuple[bool, bool]:
    return _queue_bindings.drain_pending_jobs(globals(), config, blocking=blocking)


# ── GitHub Helpers ───────────────────────────────────────────────────────────


def print_local_ci_state_footprint(*, indent: str = "") -> None:
    return _utility_command_bindings.print_local_ci_state_footprint(globals(), indent=indent)


def print_local_ci_cleanup_plan(plan: dict, *, dry_run: bool) -> None:
    return _utility_command_bindings.print_local_ci_cleanup_plan(globals(), plan, dry_run=dry_run)


def cmd_cleanup(args: argparse.Namespace) -> int:
    return _utility_command_bindings.cmd_cleanup(globals(), args)


def resolve_submission_options(
    args: argparse.Namespace, command: str
) -> tuple[dict, str, str, list[str], str, str, dict]:
    return _local_ci_command_bindings.resolve_submission_options(globals(), args, command)


def cmd_enqueue(args: argparse.Namespace) -> int:
    return _local_ci_command_bindings.cmd_enqueue(globals(), args)


def cmd_drain(_args: argparse.Namespace) -> int:
    return _local_ci_command_bindings.cmd_drain(globals(), _args)


def cmd_run(args: argparse.Namespace) -> int:
    return _local_ci_command_bindings.cmd_run(globals(), args)


def cmd_ship(args: argparse.Namespace) -> int:
    return _local_ci_command_bindings.cmd_ship(globals(), args)


def cmd_check(args: argparse.Namespace) -> int:
    return _local_ci_command_bindings.cmd_check(globals(), args)


def cmd_bump(args: argparse.Namespace) -> int:
    return _utility_command_bindings.cmd_bump(globals(), args)


def cmd_cancel(args: argparse.Namespace) -> int:
    return _utility_command_bindings.cmd_cancel(globals(), args)


def cmd_list(_args: argparse.Namespace) -> int:
    return _local_ci_command_bindings.cmd_list(globals(), _args)


def resolve_job_for_logs(job_ref: str | None) -> dict | None:
    return _utility_command_bindings.resolve_job_for_logs(globals(), job_ref)


def cmd_logs(args: argparse.Namespace) -> int:
    return _utility_command_bindings.cmd_logs(globals(), args)


def cmd_evidence(args: argparse.Namespace) -> int:
    return _utility_command_bindings.cmd_evidence(globals(), args)


def cmd_status(_args: argparse.Namespace) -> int:
    return _local_ci_command_bindings.cmd_status(globals(), _args)


def cmd_desktop_install(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_install(globals(), args)


def cmd_desktop_doctor(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_doctor(globals(), args)


def cmd_desktop_video_doctor(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_video_doctor(globals(), args)


def cmd_desktop_status(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_status(globals(), args)


def cmd_desktop_config_show(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_config_show(globals(), args)


def cmd_desktop_config_set(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_config_set(globals(), args)


def cmd_desktop_config(args: argparse.Namespace) -> int:
    return _cli_dispatch_bindings.cmd_desktop_config(globals(), args)


def cmd_desktop_serve(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_serve(globals(), args)


def cmd_desktop_recent(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_recent(globals(), args)


def cmd_desktop_proof(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_proof(globals(), args)


def cmd_desktop_publish(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_publish(globals(), args)


def cmd_desktop_verdict(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_verdict(globals(), args)


def cmd_desktop_compose_video(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_compose_video(globals(), args)


def cmd_desktop_video(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_video(globals(), args)


def cmd_desktop_cleanup(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_cleanup(globals(), args)


def windows_requires_pulp_app_selectors(args: argparse.Namespace) -> bool:
    return _desktop_command_bindings.windows_requires_pulp_app_selectors(globals(), args)


def cmd_desktop_smoke(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_smoke(globals(), args)


def cmd_desktop_click(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_click(globals(), args)


def cmd_desktop_inspect(args: argparse.Namespace) -> int:
    return _desktop_command_bindings.cmd_desktop_inspect(globals(), args)


def cmd_desktop(args: argparse.Namespace) -> int:
    return _cli_dispatch_bindings.cmd_desktop(globals(), args)


def build_parser() -> argparse.ArgumentParser:
    return build_local_ci_parser(
        priority_values=PRIORITY_VALUES,
        keep_completed_jobs=KEEP_COMPLETED_JOBS,
        epilog=__doc__,
    )


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    return _cli_dispatch_bindings.dispatch_main_command(globals(), args, parser.print_help)


if __name__ == "__main__":
    sys.exit(main())
