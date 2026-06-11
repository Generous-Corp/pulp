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
import desktop_actions as _desktop_actions  # noqa: E402
import desktop_artifacts as _desktop_artifacts  # noqa: E402
import desktop_cli as _desktop_cli  # noqa: E402
import desktop_doctor as _desktop_doctor  # noqa: E402
import linux_target as _linux_target  # noqa: E402
import macos_desktop as _macos_desktop  # noqa: E402
import queue_lifecycle as _queue_lifecycle  # noqa: E402
import queue_orchestrator as _queue_orchestrator  # noqa: E402
import execution as _execution  # noqa: E402

HEARTBEAT_INTERVAL_SECS = _execution.HEARTBEAT_INTERVAL_SECS
STUCK_IDLE_SECS = _execution.STUCK_IDLE_SECS
import reporting as _reporting  # noqa: E402
import runner_state as _runner_state  # noqa: E402
import source_prep as _source_prep  # noqa: E402
import ssh_bundle as _ssh_bundle  # noqa: E402
import target_preflight as _target_preflight  # noqa: E402
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
    return desktop_receipts_dir() / f"{target_name}.json"


def desktop_receipt_for(target_name: str) -> dict | None:
    path = desktop_target_receipt_path(target_name)
    if not path.exists():
        return None
    return json.loads(path.read_text())


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
    return _windows_probe.probe_windows_repo_checkout(
        host,
        repo_path,
        run_windows_ssh_powershell_fn=run_windows_ssh_powershell,
        windows_repo_path_is_unsafe_fn=windows_repo_path_is_unsafe,
        parse_windows_ssh_json_fn=parse_windows_ssh_json,
        ps_literal_fn=ps_literal,
    )


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
    return _windows_probe.ensure_windows_remote_repo_checkout(
        host,
        repo_path,
        remote_url=remote_url,
        bundle_name=bundle_name,
        bundle_ref=bundle_ref,
        probe_windows_repo_checkout_fn=probe_windows_repo_checkout,
        windows_repo_path_is_unsafe_fn=windows_repo_path_is_unsafe,
        windows_default_repo_checkout_path_fn=windows_default_repo_checkout_path,
        run_windows_ssh_powershell_fn=run_windows_ssh_powershell,
        parse_windows_ssh_json_fn=parse_windows_ssh_json,
        windows_contract_expand_expression_fn=windows_contract_expand_expression,
        ps_literal_fn=ps_literal,
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
    return _windows_probe.probe_windows_session_agent(
        host,
        contract,
        run_windows_ssh_powershell_fn=run_windows_ssh_powershell,
        parse_windows_ssh_json_fn=parse_windows_ssh_json,
        windows_contract_expand_expression_fn=windows_contract_expand_expression,
        ps_literal_fn=ps_literal,
    )


def probe_windows_remote_tooling(host: str) -> dict:
    return _windows_probe.probe_windows_remote_tooling(
        host,
        run_windows_ssh_powershell_fn=run_windows_ssh_powershell,
        parse_windows_ssh_json_fn=parse_windows_ssh_json,
    )


def install_windows_remote_tool(host: str, package_id: str, *, timeout: int = 900) -> None:
    return _windows_probe.install_windows_remote_tool(
        host,
        package_id,
        timeout=timeout,
        run_windows_ssh_powershell_fn=run_windows_ssh_powershell,
        ps_literal_fn=ps_literal,
    )


def ensure_windows_remote_tooling(host: str, *, install_optional: bool = False) -> dict:
    return _windows_probe.ensure_windows_remote_tooling(
        host,
        install_optional=install_optional,
        required_tools=WINDOWS_REQUIRED_REMOTE_TOOLS,
        optional_tools=WINDOWS_OPTIONAL_REMOTE_TOOLS,
        probe_windows_remote_tooling_fn=probe_windows_remote_tooling,
        install_windows_remote_tool_fn=install_windows_remote_tool,
    )


def windows_tooling_detail(probe: dict, tool_name: str, *, missing_hint: str | None = None) -> str:
    return _windows_target.windows_tooling_detail(probe, tool_name, missing_hint=missing_hint)


def windows_remote_tooling_ready(probe: dict) -> bool:
    return _windows_target.windows_remote_tooling_ready(probe, required_tools=WINDOWS_REQUIRED_REMOTE_TOOLS)


def desktop_doctor_checks(config: dict, target_name: str) -> list[dict]:
    return _desktop_doctor.desktop_doctor_checks(
        config,
        target_name,
        resolve_desktop_target_fn=resolve_desktop_target,
        desktop_target_contract_fn=desktop_target_contract,
        desktop_receipt_for_fn=desktop_receipt_for,
        macos_accessibility_trusted_fn=macos_accessibility_trusted,
        ssh_reachable_fn=ssh_reachable,
        ssh_failure_detail_fn=ssh_failure_detail,
        probe_linux_launch_backend_fn=probe_linux_launch_backend,
        probe_linux_remote_tooling_fn=probe_linux_remote_tooling,
        probe_windows_session_agent_fn=probe_windows_session_agent,
        probe_windows_remote_tooling_fn=probe_windows_remote_tooling,
        probe_windows_repo_checkout_fn=probe_windows_repo_checkout,
        platform=sys.platform,
        which_fn=shutil.which,
        probe_webdriver_endpoint_fn=probe_webdriver_endpoint,
    )


def webdriver_status_url(base_url: str) -> str:
    return _desktop_doctor.webdriver_status_url(base_url)


def probe_webdriver_endpoint(base_url: str, *, timeout: float = 5.0) -> dict:
    return _desktop_doctor.probe_webdriver_endpoint(
        base_url,
        timeout=timeout,
        request_cls=urllib.request.Request,
        urlopen_fn=urllib.request.urlopen,
    )


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
    value = (remote_url or '').strip()
    if not value:
        return None
    if value.startswith('git@github.com:'):
        repo_path = value[len('git@github.com:'):].rstrip('/')
        if repo_path.endswith('.git'):
            repo_path = repo_path[:-4]
        return f'https://github.com/{repo_path}'
    if value.startswith('https://github.com/') or value.startswith('http://github.com/'):
        prefix = 'https://github.com/' if 'github.com/' in value else None
        repo_path = value.split('github.com/', 1)[1].rstrip('/')
        if repo_path.endswith('.git'):
            repo_path = repo_path[:-4]
        return f'https://github.com/{repo_path}'
    return None


def normalize_git_remote_for_clone(remote_url: str | None) -> str | None:
    value = (remote_url or '').strip()
    if not value:
        return None
    if value.startswith('git@github.com:'):
        repo_path = value[len('git@github.com:'):].rstrip('/')
        if repo_path.endswith('.git'):
            return f'https://github.com/{repo_path}'
        return f'https://github.com/{repo_path}.git'
    if value.startswith('https://github.com/') or value.startswith('http://github.com/'):
        repo_path = value.split('github.com/', 1)[1].rstrip('/')
        if repo_path.endswith('.git'):
            return f'https://github.com/{repo_path}'
        return f'https://github.com/{repo_path}.git'
    return None


def git_origin_http_url(repo_root: Path = ROOT) -> str | None:
    run = subprocess.run(
        ['git', 'remote', 'get-url', 'origin'],
        cwd=repo_root,
        capture_output=True,
        text=True,
        check=False,
    )
    if run.returncode != 0:
        return None
    return normalize_git_remote_for_http(run.stdout.strip())


def git_origin_clone_url(repo_root: Path = ROOT) -> str | None:
    run = subprocess.run(
        ['git', 'remote', 'get-url', 'origin'],
        cwd=repo_root,
        capture_output=True,
        text=True,
        check=False,
    )
    if run.returncode != 0:
        return None
    return normalize_git_remote_for_clone(run.stdout.strip())


def _clear_directory_contents(path: Path) -> None:
    if not path.exists():
        return
    for child in path.iterdir():
        if child.name == '.git':
            continue
        if child.is_dir():
            shutil.rmtree(child, ignore_errors=True)
        else:
            child.unlink(missing_ok=True)


def _copy_directory_contents(src: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for child in src.iterdir():
        target = dest / child.name
        if child.is_dir():
            shutil.copytree(child, target, dirs_exist_ok=True)
        else:
            shutil.copy2(child, target)


def _run_git(args: list[str], *, cwd: Path, check: bool = True) -> subprocess.CompletedProcess:
    run = subprocess.run(
        ['git', *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
    )
    if check and run.returncode != 0:
        detail = (run.stderr or run.stdout or '').strip()
        raise RuntimeError(f"git {' '.join(args)} failed: {detail or run.returncode}")
    return run


def publish_report_to_branch(config: dict, report: dict) -> dict:
    return _reporting.publish_report_to_branch(
        config,
        report,
        root=ROOT,
        run_git_fn=_run_git,
        reset_local_worktree_fn=_reset_local_worktree,
        clear_directory_contents_fn=_clear_directory_contents,
        git_origin_http_url_fn=git_origin_http_url,
    )


def make_desktop_source_request(args: argparse.Namespace) -> dict:
    return _source_prep.make_desktop_source_request(
        args,
        normalize_desktop_source_mode_fn=normalize_desktop_source_mode,
        current_branch_fn=current_branch,
        current_sha_fn=current_sha,
    )


def desktop_source_cache_key(source_request: dict) -> str:
    return _source_prep.desktop_source_cache_key(source_request)


def desktop_source_root(target_name: str, source_request: dict) -> Path:
    return _source_prep.desktop_source_root(
        target_name,
        source_request,
        state_dir_fn=state_dir,
    )


def _command_path_rewrite_candidate(token: str) -> Path | None:
    return _source_prep.command_path_rewrite_candidate(token, root=ROOT)


def _rewrite_launch_command_for_mapper(command: str | None, mapper, *, windows: bool = False) -> str | None:
    return _source_prep.rewrite_launch_command_for_mapper(
        command,
        mapper,
        root=ROOT,
        windows=windows,
    )


def _windows_command_join(parts: list[str]) -> str:
    return subprocess.list2cmdline(parts)


def rewrite_launch_command_for_source_root(command: str | None, source_root: Path) -> str | None:
    return _source_prep.rewrite_launch_command_for_source_root(command, source_root, root=ROOT)


def rewrite_launch_command_for_posix_root(command: str | None, remote_root: str) -> str | None:
    return _source_prep.rewrite_launch_command_for_posix_root(command, remote_root, root=ROOT)


def rewrite_launch_command_for_windows_root(command: str | None, remote_root: str) -> str | None:
    return _source_prep.rewrite_launch_command_for_windows_root(
        command,
        remote_root,
        root=ROOT,
        windows_path_join_fn=windows_path_join,
    )


def split_windows_prepare_commands(command: str) -> list[str]:
    return _source_prep.split_windows_prepare_commands(command)


def validate_windows_prepare_commands(commands: list[str]) -> None:
    return _source_prep.validate_windows_prepare_commands(commands)


def attach_desktop_source_to_manifest(manifest: dict, source_context: dict | None) -> None:
    return _source_prep.attach_desktop_source_to_manifest(manifest, source_context)


def slugify_token(value: str, *, max_len: int = 48) -> str:
    return _reporting.slugify_token(value, max_len=max_len)


def stage_desktop_publish_report(
    config: dict,
    manifests: list[dict],
    *,
    output_dir: Path | None = None,
    label: str | None = None,
) -> dict:
    return _reporting.stage_desktop_publish_report(
        config,
        manifests,
        output_dir=output_dir,
        label=label,
        create_desktop_publish_bundle_fn=create_desktop_publish_bundle,
        now_iso_fn=now_iso,
        atomic_write_text_fn=atomic_write_text,
        write_desktop_publish_rollups_fn=write_desktop_publish_rollups,
        publish_report_to_branch_fn=publish_report_to_branch,
    )


def desktop_publish_reports(config: dict, *, limit: int | None = None) -> list[dict]:
    return _reporting.desktop_publish_reports(
        config,
        limit=limit,
        desktop_publish_root_fn=desktop_publish_root,
    )


def write_desktop_publish_rollups(config: dict) -> None:
    _reporting.write_desktop_publish_rollups(
        config,
        desktop_publish_root_fn=desktop_publish_root,
        desktop_publish_reports_fn=desktop_publish_reports,
        atomic_write_text_fn=atomic_write_text,
    )


def wait_for_path(path: Path, timeout_secs: float) -> Path:
    deadline = time.time() + timeout_secs
    while time.time() < deadline:
        if path.exists():
            return path
        time.sleep(0.1)
    raise RuntimeError(f"timed out waiting for artifact `{path}`")


def count_view_tree_nodes(node: object) -> int:
    return _desktop_actions.count_view_tree_nodes(node)


def detect_macos_app_bundle(command: str | None) -> Path | None:
    return _macos_desktop.detect_macos_app_bundle(command)


def macos_bundle_id_for_app_path(app_path: Path) -> str | None:
    return _macos_desktop.macos_bundle_id_for_app_path(app_path)


def desktop_run_manifests(config: dict, *, target_name: str | None = None, action: str | None = None) -> list[dict]:
    return _reporting.desktop_run_manifests(
        config,
        target_name=target_name,
        action=action,
        desktop_artifact_root_fn=desktop_artifact_root,
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
    return _reporting.desktop_proof_summaries(
        config,
        target_name=target_name,
        action=action,
        source_mode=source_mode,
        sha=sha,
        branch=branch,
        limit=limit,
        desktop_run_manifests_fn=desktop_run_manifests,
        desktop_run_summary_fn=desktop_run_summary,
    )


def desktop_rollup_dir(config: dict, target_name: str | None = None) -> Path:
    return _reporting.desktop_rollup_dir(
        config,
        target_name,
        desktop_artifact_root_fn=desktop_artifact_root,
    )


def write_desktop_run_rollups(config: dict, *, target_name: str | None = None) -> None:
    _reporting.write_desktop_run_rollups(
        config,
        target_name=target_name,
        desktop_rollup_dir_fn=desktop_rollup_dir,
        desktop_run_manifests_fn=desktop_run_manifests,
        desktop_run_summary_fn=desktop_run_summary,
        desktop_proof_summaries_fn=desktop_proof_summaries,
        atomic_write_text_fn=atomic_write_text,
    )


def prune_desktop_run_manifests(
    config: dict,
    *,
    target_name: str | None = None,
    older_than_days: int | None = None,
    keep_last: int | None = None,
) -> list[Path]:
    return _reporting.prune_desktop_run_manifests(
        config,
        target_name=target_name,
        older_than_days=older_than_days,
        keep_last=keep_last,
        desktop_run_manifests_fn=desktop_run_manifests,
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
    return _source_prep.local_worktree_matches(path, sha, run_fn=subprocess.run)


def _reset_local_worktree(path: Path) -> None:
    return _source_prep.reset_local_worktree(
        path,
        root=ROOT,
        run_fn=subprocess.run,
    )


def prepare_macos_exact_sha_source(
    bundle_dir: Path,
    target_name: str,
    command: str,
    source_request: dict,
) -> dict:
    return _source_prep.prepare_macos_exact_sha_source(
        bundle_dir,
        target_name,
        command,
        source_request,
        root=ROOT,
        desktop_source_root_fn=desktop_source_root,
        local_worktree_matches_fn=_local_worktree_matches,
        reset_local_worktree_fn=_reset_local_worktree,
        run_fn=subprocess.run,
        run_logged_command_fn=run_logged_command,
        tail_lines_fn=tail_lines,
        rewrite_launch_command_for_source_root_fn=rewrite_launch_command_for_source_root,
    )


def prepare_linux_exact_sha_source(
    bundle_dir: Path,
    target_name: str,
    host: str,
    command: str,
    source_request: dict,
) -> dict:
    return _source_prep.prepare_linux_exact_sha_source(
        bundle_dir,
        target_name,
        host,
        command,
        source_request,
        sync_job_bundle_to_ssh_host_fn=sync_job_bundle_to_ssh_host,
        git_origin_clone_url_fn=git_origin_clone_url,
        desktop_source_cache_key_fn=desktop_source_cache_key,
        root=ROOT,
        run_fn=subprocess.run,
        fetch_ssh_artifact_fn=fetch_ssh_artifact,
        rewrite_launch_command_for_posix_root_fn=rewrite_launch_command_for_posix_root,
    )


def prepare_windows_exact_sha_source(
    bundle_dir: Path,
    target_name: str,
    host: str,
    command: str,
    source_request: dict,
) -> dict:
    return _source_prep.prepare_windows_exact_sha_source(
        bundle_dir,
        target_name,
        host,
        command,
        source_request,
        sync_job_bundle_to_ssh_host_fn=sync_job_bundle_to_ssh_host,
        git_origin_clone_url_fn=git_origin_clone_url,
        desktop_source_cache_key_fn=desktop_source_cache_key,
        root=ROOT,
        ps_literal_fn=ps_literal,
        windows_contract_expand_expression_fn=windows_contract_expand_expression,
        split_windows_prepare_commands_fn=split_windows_prepare_commands,
        validate_windows_prepare_commands_fn=validate_windows_prepare_commands,
        run_windows_ssh_powershell_fn=run_windows_ssh_powershell,
        windows_ssh_fetch_file_fn=windows_ssh_fetch_file,
        rewrite_launch_command_for_windows_root_fn=rewrite_launch_command_for_windows_root,
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
) -> dict:
    bundle_dir = create_desktop_run_bundle(config, "mac", action_name)
    action_paths = _desktop_actions.desktop_action_artifact_paths(bundle_dir, output_path)
    screenshot_path = action_paths["screenshot"]
    before_screenshot_path = action_paths["before_screenshot"]
    diff_screenshot_path = action_paths["diff_screenshot"]
    ui_snapshot_path = action_paths["ui_snapshot"]
    log_path = action_paths["stdout"]
    err_path = action_paths["stderr"]

    interaction_requested = _desktop_actions.desktop_interaction_requested(
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
    )
    use_pulp_app_automation = bool(pulp_app_automation and interaction_requested)
    if use_pulp_app_automation and bundle_id:
        raise RuntimeError("Pulp app automation requires a direct --command launch so automation env vars can be injected.")
    if interaction_requested and not use_pulp_app_automation and not macos_accessibility_trusted():
        raise RuntimeError("macOS desktop interaction requires Accessibility access for the terminal/runner.")
    if (click_view_id or click_view_type or click_view_text or click_view_label) and not capture_ui_snapshot and not use_pulp_app_automation:
        raise RuntimeError("View-targeted click requires --capture-ui-snapshot so the app writes a ViewInspector tree.")

    started_at = now_iso()
    source_context = dict(source_request or {})
    launch_cwd: str | None = None
    launch_command = command
    if source_context.get("mode") == "exact-sha":
        if bundle_id:
            raise RuntimeError("Exact-SHA desktop source mode currently requires --command, not --bundle-id.")
        if not command:
            raise RuntimeError("Exact-SHA desktop source mode requires --command.")
        source_context = prepare_macos_exact_sha_source(bundle_dir, "mac", command, source_context)
        launch_cwd = source_context.get("launch_cwd")
        launch_command = source_context.get("launch_command") or command
    proc = None
    pid = None
    try:
        if bundle_id:
            if capture_ui_snapshot:
                raise RuntimeError(
                    "UI snapshot capture currently requires a direct launch command so PULP_VIEW_TREE_OUT can be injected."
                )
            log_path.write_text("")
            err_path.write_text("")
            quit_macos_bundle_id(bundle_id)
            time.sleep(0.2)
            subprocess.run(["open", "-b", bundle_id], capture_output=True, text=True, check=True)
            time.sleep(0.75)
            activate_macos_bundle_id(bundle_id)
            time.sleep(0.75)
            pid, window = wait_for_macos_bundle_window(bundle_id, timeout_secs)
            launch_descriptor = {"bundle_id": bundle_id}
        else:
            args = shlex.split(launch_command or "")
            if not args:
                raise ValueError("Desktop smoke requires either --command or --bundle-id.")
            app_bundle = detect_macos_app_bundle(launch_command)
            if app_bundle is not None:
                if capture_ui_snapshot:
                    raise RuntimeError(
                        "UI snapshot capture currently requires a direct launch command so PULP_VIEW_TREE_OUT can be injected."
                    )
                inferred_bundle_id = macos_bundle_id_for_app_path(app_bundle)
                if not inferred_bundle_id:
                    raise RuntimeError(f"Could not determine bundle id for app bundle `{app_bundle}`")
                log_path.write_text("")
                err_path.write_text("")
                quit_macos_bundle_id(inferred_bundle_id)
                time.sleep(0.2)
                subprocess.run(["open", "-a", str(app_bundle)], capture_output=True, text=True, check=True)
                time.sleep(0.75)
                activate_macos_bundle_id(inferred_bundle_id)
                time.sleep(0.75)
                pid, window = wait_for_macos_bundle_window(inferred_bundle_id, timeout_secs)
                launch_descriptor = {"bundle_id": inferred_bundle_id, "app_path": str(app_bundle)}
            else:
                stdout_handle = log_path.open("w")
                stderr_handle = err_path.open("w")
                env = os.environ.copy()
                if capture_ui_snapshot:
                    env["PULP_VIEW_TREE_OUT"] = str(ui_snapshot_path)
                if use_pulp_app_automation:
                    if click_point:
                        env["PULP_AUTOMATION_CLICK_POINT"] = click_point
                    if click_view_id:
                        env["PULP_AUTOMATION_CLICK_VIEW_ID"] = click_view_id
                    if click_view_type:
                        env["PULP_AUTOMATION_CLICK_VIEW_TYPE"] = click_view_type
                    if click_view_text:
                        env["PULP_AUTOMATION_CLICK_VIEW_TEXT"] = click_view_text
                    if click_view_label:
                        env["PULP_AUTOMATION_CLICK_VIEW_LABEL"] = click_view_label
                    if capture_before:
                        env["PULP_AUTOMATION_BEFORE_OUT"] = str(before_screenshot_path)
                    env["PULP_AUTOMATION_AFTER_OUT"] = str(screenshot_path)
                    env["PULP_AUTOMATION_DELAY_MS"] = "1000"
                    env["PULP_AUTOMATION_AFTER_DELAY_MS"] = str(max(0, int(settle_secs * 1000.0)))
                    env["PULP_AUTOMATION_EXIT_AFTER"] = "1"
                try:
                    proc = subprocess.Popen(
                        args,
                        stdout=stdout_handle,
                        stderr=stderr_handle,
                        env=env,
                        cwd=launch_cwd,
                    )
                finally:
                    stdout_handle.close()
                    stderr_handle.close()
                pid = proc.pid
                window = wait_for_macos_window(proc.pid, timeout_secs)
                launch_descriptor = {"command": args}

        inspector_summary = None
        view_tree = None
        content_size = _desktop_actions.content_size_from_window(window)
        if capture_ui_snapshot and not use_pulp_app_automation:
            wait_for_path(ui_snapshot_path, timeout_secs)
            view_tree = json.loads(ui_snapshot_path.read_text())
            content_size = _desktop_actions.content_size_from_view_tree(view_tree, content_size)
            inspector_summary = _desktop_actions.view_tree_inspector_summary(view_tree)

        interaction_summary = None
        if use_pulp_app_automation:
            if capture_before:
                wait_for_path(before_screenshot_path, timeout_secs)
            wait_for_path(screenshot_path, timeout_secs)
            if capture_ui_snapshot:
                wait_for_path(ui_snapshot_path, timeout_secs)
                view_tree = json.loads(ui_snapshot_path.read_text())
                content_size = _desktop_actions.content_size_from_view_tree(view_tree, content_size)
                inspector_summary = _desktop_actions.view_tree_inspector_summary(view_tree)
            interaction_summary = _desktop_actions.pulp_app_interaction_summary(
                click_point=click_point,
                click_view_id=click_view_id,
                click_view_type=click_view_type,
                click_view_text=click_view_text,
                click_view_label=click_view_label,
            )
        else:
            if interaction_requested and capture_before:
                capture_macos_window(int(window["windowId"]), before_screenshot_path)

            if interaction_requested:
                if click_point:
                    content_point = parse_coordinate_pair(click_point, flag_name="--click")
                else:
                    content_point = resolve_view_tree_click_point(
                        view_tree or {},
                        view_id=click_view_id,
                        view_type=click_view_type,
                        view_text=click_view_text,
                        view_label=click_view_label,
                    )
                screen_point = screen_point_for_content_point(window, content_size, content_point)
                activation_payload = activate_macos_pid(int(pid or 0)) if pid else {"activated": False}
                dispatch_payload = dispatch_macos_click(*screen_point)
                interaction_summary = {
                    "mode": "desktop-event",
                    "click": {
                        "content_point": {"x": content_point[0], "y": content_point[1]},
                        "screen_point": {"x": screen_point[0], "y": screen_point[1]},
                        "selector": _desktop_actions.desktop_click_selector(
                            click_view_id=click_view_id,
                            click_view_type=click_view_type,
                            click_view_text=click_view_text,
                            click_view_label=click_view_label,
                            include_point=False,
                        ),
                        "activation": activation_payload,
                        "dispatch": dispatch_payload,
                    }
                }
                if settle_secs > 0:
                    time.sleep(settle_secs)

            try:
                capture_macos_window(int(window["windowId"]), screenshot_path)
            except RuntimeError:
                active_bundle_id = bundle_id or launch_descriptor.get("bundle_id")
                if not active_bundle_id:
                    raise
                pid, window = wait_for_macos_bundle_window(active_bundle_id, min(timeout_secs, 2.0))
                capture_macos_window(int(window["windowId"]), screenshot_path)
        manifest = {
            "target": "mac",
            "adapter": "macos-local",
            "action": action_name,
            "label": label or (bundle_id or Path((launch_command or "").split()[0]).stem),
            "pid": pid,
            "started_at": started_at,
            "completed_at": now_iso(),
            "window": window,
            **launch_descriptor,
            "artifacts": {
                "bundle_dir": str(bundle_dir),
                "screenshot": str(screenshot_path),
                "stdout": str(log_path),
                "stderr": str(err_path),
            },
        }
        if capture_before and interaction_requested:
            manifest["artifacts"]["before_screenshot"] = str(before_screenshot_path)
            if before_screenshot_path.exists() and screenshot_path.exists():
                manifest["artifacts"]["image_change"] = image_change_summary(
                    before_screenshot_path,
                    screenshot_path,
                    diff_output_path=diff_screenshot_path,
                )
                if diff_screenshot_path.exists():
                    manifest["artifacts"]["diff_screenshot"] = str(diff_screenshot_path)
        if inspector_summary is not None:
            manifest["artifacts"]["ui_snapshot"] = str(ui_snapshot_path)
            manifest["inspector"] = inspector_summary
        if interaction_summary is not None:
            manifest["interaction"] = interaction_summary
        attach_desktop_source_to_manifest(manifest, source_context or source_request)
        atomic_write_text(bundle_dir / "manifest.json", json.dumps(manifest, indent=2) + "\n")
        write_desktop_run_rollups(config, target_name="mac")
        write_desktop_run_rollups(config)
        return manifest
    finally:
        if proc is not None:
            terminate_process(proc)
        else:
            active_bundle_id = bundle_id
            if not active_bundle_id and 'launch_descriptor' in locals():
                active_bundle_id = launch_descriptor.get("bundle_id")
            if active_bundle_id:
                quit_macos_bundle_id(active_bundle_id)


def default_desktop_label(command: str | None, *, bundle_id: str | None = None) -> str:
    return _desktop_actions.default_desktop_label(command, bundle_id=bundle_id)


def remote_linux_bundle_relpath(target_name: str, action_name: str, bundle_dir: Path) -> str:
    return _linux_target.remote_linux_bundle_relpath(target_name, action_name, bundle_dir)


def fetch_ssh_artifact(host: str, remote_path: str, local_path: Path, *, optional: bool = False, timeout: int = 60) -> bool:
    local_path.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        ["scp", f"{host}:{remote_path}", str(local_path)],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if result.returncode == 0 and local_path.exists():
        return True
    if optional:
        return False
    detail = result.stderr.strip() or result.stdout.strip() or f"scp exited {result.returncode}"
    raise RuntimeError(f"Failed to copy `{remote_path}` from {host}: {detail}")


def cleanup_remote_ssh_dir(host: str, remote_dir_expr: str) -> None:
    try:
        ssh_command_result(host, f"rm -rf {remote_dir_expr}", timeout=20)
    except Exception:
        pass


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
    host = ensure_host_reachable(target_name, target, config.get("defaults", {}))
    if not host:
        raise RuntimeError(f"Desktop target `{target_name}` is not reachable over SSH.")
    repo_path = target.get("repo_path")
    if not repo_path:
        raise RuntimeError(f"Desktop target `{target_name}` is missing repo_path.")
    launch_backend = probe_linux_launch_backend(host)
    if launch_backend.get("mode") == "missing":
        raise RuntimeError(
            f"Desktop target `{target_name}` needs xvfb-run or an existing desktop display session."
        )
    interaction_requested = _desktop_actions.desktop_interaction_requested(
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
    )
    if not pulp_app_automation:
        if capture_ui_snapshot:
            raise RuntimeError("linux-xvfb desktop inspect supports UI snapshots only with --pulp-app-automation.")
        if any([click_view_id, click_view_type, click_view_text, click_view_label]):
            raise RuntimeError("linux-xvfb view-target selectors currently require --pulp-app-automation.")

    bundle_dir = create_desktop_run_bundle(config, target_name, action_name)
    action_paths = _desktop_actions.desktop_action_artifact_paths(bundle_dir, output_path)
    screenshot_path = action_paths["screenshot"]
    before_screenshot_path = action_paths["before_screenshot"]
    diff_screenshot_path = action_paths["diff_screenshot"]
    ui_snapshot_path = action_paths["ui_snapshot"]
    log_path = action_paths["stdout"]
    err_path = action_paths["stderr"]
    pid_path = bundle_dir / "pid.txt"
    window_id_path = bundle_dir / "window-id.txt"
    window_title_path = bundle_dir / "window-title.txt"
    started_at = now_iso()
    remote_bundle_relpath = remote_linux_bundle_relpath(target_name, action_name, bundle_dir)
    remote_bundle_copy_root = f"~/{remote_bundle_relpath}"
    remote_bundle_cleanup_expr = f'"$HOME/{remote_bundle_relpath}"'
    source_context = dict(source_request or {})
    if source_context.get("mode") == "exact-sha":
        source_context = prepare_linux_exact_sha_source(bundle_dir, target_name, host, command, source_context)
    launch_cwd = source_context.get("launch_cwd") or repo_path
    launch_command = source_context.get("launch_command") or command
    if pulp_app_automation:
        remote_cmd = build_linux_xvfb_remote_command(
            repo_path,
            remote_bundle_relpath,
            launch_command,
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
    else:
        remote_cmd = build_linux_window_driver_remote_command(
            repo_path,
            remote_bundle_relpath,
            launch_command,
            launch_backend=launch_backend,
            launch_cwd=launch_cwd,
            click_point=click_point,
            capture_before=capture_before,
            settle_secs=settle_secs,
        )

    remote_cmd = 'export PATH="$HOME/.local/bin:$PATH"; ' + remote_cmd
    run = subprocess.run(
        ["ssh", host, "bash", "-lc", shlex.quote(remote_cmd)],
        capture_output=True,
        text=True,
        timeout=max(30, int(timeout_secs + settle_secs + 20)),
    )
    log_path.write_text(run.stdout or "")
    err_path.write_text(run.stderr or "")

    remote_screenshot = remote_bundle_copy_root + "/screenshots/window.png"
    remote_before = remote_bundle_copy_root + "/screenshots/before.png"
    remote_ui = remote_bundle_copy_root + "/ui-tree.json"
    remote_stdout = remote_bundle_copy_root + "/stdout.log"
    remote_stderr = remote_bundle_copy_root + "/stderr.log"
    remote_pid = remote_bundle_copy_root + "/pid.txt"
    remote_window_id = remote_bundle_copy_root + "/window-id.txt"
    remote_window_title = remote_bundle_copy_root + "/window-title.txt"

    try:
        fetch_ssh_artifact(host, remote_stdout, log_path, optional=True)
        fetch_ssh_artifact(host, remote_stderr, err_path, optional=True)
        fetch_ssh_artifact(host, remote_screenshot, screenshot_path)
        fetch_ssh_artifact(host, remote_pid, pid_path, optional=True)
        fetch_ssh_artifact(host, remote_window_id, window_id_path, optional=True)
        fetch_ssh_artifact(host, remote_window_title, window_title_path, optional=True)
        if capture_before:
            fetch_ssh_artifact(host, remote_before, before_screenshot_path, optional=not pulp_app_automation)
        if capture_ui_snapshot:
            fetch_ssh_artifact(host, remote_ui, ui_snapshot_path)
    finally:
        cleanup_remote_ssh_dir(host, remote_bundle_cleanup_expr)

    if run.returncode != 0:
        detail = err_path.read_text(errors="replace").strip() or log_path.read_text(errors="replace").strip() or f"remote command exited {run.returncode}"
        raise RuntimeError(detail)

    pid_value = None
    if pid_path.exists():
        try:
            pid_value = int(pid_path.read_text().strip())
        except ValueError:
            pid_value = None

    manifest = {
        "target": target_name,
        "adapter": target["adapter"],
        "action": action_name,
        "label": label or default_desktop_label(command),
        "pid": pid_value,
        "host": host,
        "repo_path": repo_path,
        "command": launch_command,
        "started_at": started_at,
        "completed_at": now_iso(),
        "artifacts": {
            "bundle_dir": str(bundle_dir),
            "screenshot": str(screenshot_path),
            "stdout": str(log_path),
            "stderr": str(err_path),
            "remote_bundle_dir": remote_bundle_copy_root,
        },
    }
    if window_id_path.exists() or window_title_path.exists():
        manifest["window"] = {}
        if window_id_path.exists():
            manifest["window"]["window_id"] = window_id_path.read_text().strip()
        if window_title_path.exists():
            manifest["window"]["title"] = window_title_path.read_text().strip()
    if capture_before and before_screenshot_path.exists() and screenshot_path.exists():
        manifest["artifacts"]["before_screenshot"] = str(before_screenshot_path)
        manifest["artifacts"]["image_change"] = image_change_summary(
            before_screenshot_path,
            screenshot_path,
            diff_output_path=diff_screenshot_path,
        )
        if diff_screenshot_path.exists():
            manifest["artifacts"]["diff_screenshot"] = str(diff_screenshot_path)
    if capture_ui_snapshot and ui_snapshot_path.exists():
        view_tree = json.loads(ui_snapshot_path.read_text())
        manifest["artifacts"]["ui_snapshot"] = str(ui_snapshot_path)
        manifest["inspector"] = _desktop_actions.view_tree_inspector_summary(view_tree)
    if interaction_requested:
        if pulp_app_automation:
            manifest["interaction"] = _desktop_actions.pulp_app_interaction_summary(
                click_point=click_point,
                click_view_id=click_view_id,
                click_view_type=click_view_type,
                click_view_text=click_view_text,
                click_view_label=click_view_label,
            )
        else:
            click_summary = {"point": click_point}
            if click_point:
                content_x, content_y = parse_coordinate_pair(click_point, flag_name="--click")
                click_summary["content_point"] = {"x": content_x, "y": content_y}
            manifest["interaction"] = {"mode": "x11-window-driver", "click": click_summary}
    attach_desktop_source_to_manifest(manifest, source_context or source_request)
    atomic_write_text(bundle_dir / "manifest.json", json.dumps(manifest, indent=2) + "\n")
    write_desktop_run_rollups(config, target_name=target_name)
    write_desktop_run_rollups(config)
    return manifest


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
    host = ensure_host_reachable(target_name, target, config.get("defaults", {}))
    if not host:
        raise RuntimeError(f"Desktop target `{target_name}` is not reachable over SSH.")
    if not target.get("repo_path"):
        raise RuntimeError(f"Desktop target `{target_name}` is missing repo_path.")

    receipt = desktop_receipt_for(target_name)
    if not receipt:
        raise RuntimeError(f"Desktop target `{target_name}` is not installed. Run `pulp ci-local desktop install {target_name}`.")

    contract = receipt.get("contract") or desktop_target_contract(target_name, target)
    probe = probe_windows_session_agent(host, contract)
    if not (
        probe.get("task_present")
        and probe.get("agent_root_exists")
        and probe.get("jobs_dir_exists")
        and probe.get("results_dir_exists")
        and probe.get("script_exists")
    ):
        raise RuntimeError(
            f"Desktop target `{target_name}` is not bootstrapped. Run `pulp ci-local desktop install {target_name}`."
        )
    if not windows_desktop_session_user(probe):
        raise RuntimeError(
            f"Desktop target `{target_name}` has no logged-in desktop session. Log into the target desktop, then retry."
        )
    if not pulp_app_automation:
        if capture_ui_snapshot:
            raise RuntimeError(
                f"Desktop target `{target_name}` currently supports --capture-ui-snapshot only with --pulp-app-automation."
            )
        if any([click_view_id, click_view_type, click_view_text, click_view_label]):
            raise RuntimeError(
                f"Desktop target `{target_name}` currently supports view-target selectors only with --pulp-app-automation."
            )

    bundle_dir = create_desktop_run_bundle(config, target_name, action_name)
    action_paths = _desktop_actions.desktop_action_artifact_paths(bundle_dir, output_path)
    screenshot_path = action_paths["screenshot"]
    before_screenshot_path = action_paths["before_screenshot"]
    diff_screenshot_path = action_paths["diff_screenshot"]
    ui_snapshot_path = action_paths["ui_snapshot"]
    log_path = action_paths["stdout"]
    err_path = action_paths["stderr"]
    agent_manifest_path = bundle_dir / "agent-manifest.json"
    started_at = now_iso()
    interaction_requested = _desktop_actions.desktop_interaction_requested(
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
    )
    source_context = dict(source_request or {})
    if source_context.get("mode") == "exact-sha":
        source_context = prepare_windows_exact_sha_source(bundle_dir, target_name, host, command, source_context)
    launch_cwd = source_context.get("launch_cwd") or target["repo_path"]
    launch_command = source_context.get("launch_command") or command

    request = build_windows_session_agent_request(
        target_name,
        contract,
        launch_command,
        repo_path=launch_cwd,
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
    )
    remote_request_path = windows_path_join(contract["jobs_dir"], f"{request['job_id']}.json")
    windows_ssh_write_text(host, remote_request_path, json.dumps(request, indent=2) + "\n")
    try:
        start_windows_session_agent_task(host, contract)
        deadline = time.time() + timeout_secs + settle_secs + 15.0
        remote_manifest: dict | None = None
        while time.time() < deadline:
            remote_manifest = windows_ssh_read_json(
                host,
                request["outputs"]["manifest"],
                timeout=15,
                optional=True,
            )
            if remote_manifest is not None:
                break
            time.sleep(0.5)
        if remote_manifest is None:
            raise RuntimeError(
                f"Timed out waiting for Windows desktop agent result for `{target_name}` ({request['job_id']})."
            )

        agent_manifest_path.parent.mkdir(parents=True, exist_ok=True)
        atomic_write_text(agent_manifest_path, json.dumps(remote_manifest, indent=2) + "\n")

        fetch_stdout = windows_ssh_fetch_file(
            host,
            request["outputs"]["stdout"],
            log_path,
            optional=True,
            timeout=30,
        )
        fetch_stderr = windows_ssh_fetch_file(
            host,
            request["outputs"]["stderr"],
            err_path,
            optional=True,
            timeout=30,
        )
        if not fetch_stdout:
            log_path.write_text("")
        if not fetch_stderr:
            err_path.write_text("")
        windows_ssh_fetch_file(host, request["outputs"]["screenshot"], screenshot_path, timeout=60)
        if capture_before:
            windows_ssh_fetch_file(
                host,
                request["outputs"]["before_screenshot"],
                before_screenshot_path,
                optional=False,
                timeout=60,
            )
        if capture_ui_snapshot:
            windows_ssh_fetch_file(
                host,
                request["outputs"]["ui_snapshot"],
                ui_snapshot_path,
                optional=False,
                timeout=30,
            )
    finally:
        windows_ssh_remove_path(host, remote_request_path)
        windows_ssh_remove_path(host, request["outputs"]["result_root"])

    status = remote_manifest.get("status") or "error"
    error_detail = remote_manifest.get("error")
    manifest = {
        "target": target_name,
        "adapter": target["adapter"],
        "action": action_name,
        "label": label or default_desktop_label(command),
        "pid": remote_manifest.get("pid"),
        "host": host,
        "repo_path": target["repo_path"],
        "command": launch_command,
        "started_at": started_at,
        "completed_at": now_iso(),
        "window": remote_manifest.get("window"),
        "artifacts": {
            "bundle_dir": str(bundle_dir),
            "screenshot": str(screenshot_path),
            "stdout": str(log_path),
            "stderr": str(err_path),
            "agent_manifest": str(agent_manifest_path),
        },
        "agent_status": status,
    }
    if capture_before and before_screenshot_path.exists() and screenshot_path.exists():
        manifest["artifacts"]["before_screenshot"] = str(before_screenshot_path)
        manifest["artifacts"]["image_change"] = image_change_summary(
            before_screenshot_path,
            screenshot_path,
            diff_output_path=diff_screenshot_path,
        )
        if diff_screenshot_path.exists():
            manifest["artifacts"]["diff_screenshot"] = str(diff_screenshot_path)
    if capture_ui_snapshot and ui_snapshot_path.exists():
        view_tree = json.loads(ui_snapshot_path.read_text())
        manifest["artifacts"]["ui_snapshot"] = str(ui_snapshot_path)
        manifest["inspector"] = _desktop_actions.view_tree_inspector_summary(view_tree)
    remote_interaction = remote_manifest.get("interaction")
    if remote_interaction:
        manifest["interaction"] = remote_interaction
    elif interaction_requested:
        manifest["interaction"] = _desktop_actions.pulp_app_interaction_summary(
            click_point=click_point,
            click_view_id=click_view_id,
            click_view_type=click_view_type,
            click_view_text=click_view_text,
            click_view_label=click_view_label,
        )
        if not pulp_app_automation:
            manifest["interaction"]["mode"] = "window-capture"
    attach_desktop_source_to_manifest(manifest, source_context or source_request)
    atomic_write_text(bundle_dir / "manifest.json", json.dumps(manifest, indent=2) + "\n")
    write_desktop_run_rollups(config, target_name=target_name)
    write_desktop_run_rollups(config)
    if status != "pass":
        detail = error_detail or f"Windows desktop agent returned status `{status}`"
        raise RuntimeError(detail)
    return manifest


def default_priority_for(command: str, config: dict) -> str:
    return _queue_orchestrator.default_priority_for(command, config)


def make_fingerprint(branch: str, sha: str, targets: list[str], validation: str) -> str:
    return _queue_orchestrator.make_fingerprint(branch, sha, targets, validation)


def make_job(
    branch: str,
    sha: str,
    priority: str,
    targets: list[str],
    mode: str,
    validation: str,
    submission: dict | None = None,
) -> dict:
    return _queue_orchestrator.make_job(
        branch,
        sha,
        priority,
        targets,
        mode,
        validation,
        submission=submission,
        now_iso_fn=now_iso,
        uuid_hex_fn=lambda: uuid.uuid4().hex,
        root=ROOT,
        validate_branch_fn=validate_ci_branch_name,
    )


def supersedence_key(job: dict) -> tuple[str, tuple[str, ...], str]:
    return _queue_orchestrator.supersedence_key(job)


def supersedence_identity_key(job: dict) -> tuple[str, str, str]:
    return _queue_orchestrator.supersedence_identity_key(job)


def jobs_share_supersedence_scope(newer_job: dict, older_job: dict) -> bool:
    return _queue_orchestrator.jobs_share_supersedence_scope(newer_job, older_job)


def job_has_narrower_same_identity_scope(newer_job: dict, older_job: dict) -> bool:
    return _queue_orchestrator.job_has_narrower_same_identity_scope(newer_job, older_job)


def supersedence_reason(newer_job: dict, older_job: dict) -> str | None:
    return _queue_orchestrator.supersedence_reason(newer_job, older_job)


def supersedence_result(job: dict, superseded_by: str, reason: str) -> dict:
    return _queue_orchestrator.supersedence_result(job, superseded_by, reason, now_iso_fn=now_iso)


def supersede_job_unlocked(job: dict, superseded_by: str, reason: str) -> None:
    _queue_lifecycle.complete_superseded_job_unlocked(
        job,
        superseded_by,
        reason,
        supersedence_result_fn=supersedence_result,
        save_result_fn=save_result,
        complete_job_with_result_unlocked_fn=_queue_orchestrator.complete_job_with_result_unlocked,
    )


def cancellation_result(job: dict, reason: str) -> dict:
    return _queue_orchestrator.cancellation_result(job, reason, now_iso_fn=now_iso)


def cancel_job_unlocked(job: dict, reason: str = "operator_canceled") -> None:
    _queue_lifecycle.complete_canceled_job_unlocked(
        job,
        reason,
        cancellation_result_fn=cancellation_result,
        save_result_fn=save_result,
        complete_job_with_result_unlocked_fn=_queue_orchestrator.complete_job_with_result_unlocked,
    )


def summarize_job(job: dict) -> str:
    return _queue_orchestrator.summarize_job(job)


def bump_queue_command_result_line(result: dict, job_ref: str) -> tuple[int, str]:
    return _queue_orchestrator.bump_queue_command_result_line(result, job_ref)


def cancel_queue_command_result_line(result: dict, job_ref: str) -> tuple[int, str]:
    return _queue_orchestrator.cancel_queue_command_result_line(result, job_ref)


def enqueue_command_result_line(job: dict, *, created: bool) -> str:
    return _queue_orchestrator.enqueue_command_result_line(job, created=created)


def drain_runner_active_line(runner_info: dict | None) -> str:
    return _queue_orchestrator.drain_runner_active_line(runner_info)


def summarize_active_targets(active_targets: dict | None, preferred_order: list[str] | None = None) -> str:
    return _queue_orchestrator.summarize_active_targets(active_targets, preferred_order)


def status_active_targets(job: dict, runner_info: dict | None = None) -> dict | None:
    return _queue_orchestrator.status_active_targets(job, runner_info)


def status_target_states(job: dict, active_targets: dict | None) -> list[tuple[str, dict]]:
    return _queue_orchestrator.status_target_states(job, active_targets)


def status_submission_lines(job: dict) -> list[str]:
    return _queue_orchestrator.status_submission_lines(job)


def target_state_detail_parts(state: dict) -> list[str]:
    return _queue_orchestrator.target_state_detail_parts(state)


def status_target_detail_lines(job: dict, active_targets: dict | None) -> list[str]:
    return _queue_orchestrator.status_target_detail_lines(job, active_targets)


def initial_target_state(job_id: str, target_name: str, *, started_at: str) -> dict:
    return _queue_orchestrator.initial_target_state(
        started_at=started_at,
        log_path=str(target_log_path(job_id, target_name)),
    )


def updated_target_state(previous_state: dict | None, fields: dict) -> dict:
    return _queue_orchestrator.updated_target_state(previous_state, fields)


def completed_target_state(
    job_id: str,
    target_name: str,
    result: dict,
    previous_state: dict | None,
    *,
    completed_at: str,
) -> dict:
    return _queue_orchestrator.completed_target_state(
        result,
        previous_state,
        completed_at=completed_at,
        default_log_path=str(target_log_path(job_id, target_name)),
    )


def target_state_snapshot(target_states: dict[str, dict]) -> dict | None:
    return _queue_orchestrator.target_state_snapshot(target_states)


def status_runner_line(runner_info: dict | None) -> str:
    return _queue_orchestrator.status_runner_line(runner_info)


def recent_completed_status_line(job: dict, result: dict) -> str:
    return _queue_orchestrator.recent_completed_status_line(job, result)


def recent_completed_missing_result_line(job: dict) -> str:
    return _queue_orchestrator.recent_completed_missing_result_line(job)


def result_validation_line(result: dict) -> str | None:
    return _queue_orchestrator.result_validation_line(result)


def result_execution_line(result: dict) -> str:
    return _queue_orchestrator.result_execution_line(result)


def target_result_line(item: dict) -> str:
    return _queue_orchestrator.target_result_line(item)


def result_target_lines(result: dict) -> list[str]:
    return _queue_orchestrator.result_target_lines(result)


def result_overall_line(result: dict) -> str:
    return _queue_orchestrator.result_overall_line(result)


def missing_job_logs_line() -> str:
    return _queue_orchestrator.missing_job_logs_line()


def missing_log_files_line(job: dict) -> str:
    return _queue_orchestrator.missing_log_files_line(job)


def job_logs_header_line(job: dict) -> str:
    return _queue_orchestrator.job_logs_header_line(job)


def log_section_header_line(target: str) -> str:
    return _queue_orchestrator.log_section_header_line(target)


def empty_log_line() -> str:
    return _queue_orchestrator.empty_log_line()


def upsert_job_active_targets_unlocked(queue: list[dict], job_id: str, active_targets: dict | None) -> bool:
    return _queue_orchestrator.upsert_job_active_targets_unlocked(
        queue,
        job_id,
        active_targets,
        now_iso_fn=now_iso,
    )


def update_job_active_targets(job_id: str, active_targets: dict | None) -> None:
    _queue_lifecycle.update_job_active_targets_locked(
        job_id,
        active_targets,
        queue_lock_path_fn=queue_lock_path,
        file_lock_fn=file_lock,
        load_queue_unlocked_fn=load_queue_unlocked,
        upsert_job_active_targets_unlocked_fn=upsert_job_active_targets_unlocked,
        save_queue_unlocked_fn=save_queue_unlocked,
    )


def enqueue_job(
    branch: str,
    sha: str,
    priority: str,
    targets: list[str],
    mode: str,
    validation: str,
    submission: dict | None = None,
) -> tuple[dict, bool]:
    return _queue_lifecycle.enqueue_job_locked(
        branch,
        sha,
        priority,
        targets,
        mode,
        validation,
        submission=submission,
        queue_lock_path_fn=queue_lock_path,
        file_lock_fn=file_lock,
        load_queue_unlocked_fn=load_queue_unlocked,
        reconcile_running_jobs_unlocked_fn=reconcile_running_jobs_unlocked,
        save_queue_unlocked_fn=save_queue_unlocked,
        normalize_priority_fn=normalize_priority,
        normalize_validation_mode_fn=normalize_validation_mode,
        make_fingerprint_fn=make_fingerprint,
        find_active_job_by_fingerprint_unlocked_fn=_queue_orchestrator.find_active_job_by_fingerprint_unlocked,
        bump_pending_job_priority_unlocked_fn=lambda existing, requested_priority: _queue_orchestrator.bump_pending_job_priority_unlocked(
            existing,
            requested_priority,
            now_iso_fn=now_iso,
        ),
        make_job_fn=make_job,
        pending_supersedence_candidates_unlocked_fn=_queue_orchestrator.pending_supersedence_candidates_unlocked,
        supersede_job_unlocked_fn=supersede_job_unlocked,
        trim_completed_jobs_fn=trim_completed_jobs,
        normalize_job_fn=normalize_job,
    )


def trim_completed_jobs_with_removed_ids(queue: list[dict]) -> tuple[list[dict], set[str]]:
    return _queue_orchestrator.trim_completed_jobs_with_removed_ids(
        queue,
        keep_completed_jobs=KEEP_COMPLETED_JOBS,
    )


def trim_completed_jobs(queue: list[dict]) -> list[dict]:
    return _queue_orchestrator.trim_completed_jobs(
        queue,
        keep_completed_jobs=KEEP_COMPLETED_JOBS,
    )


def bump_queue_command_job(job_ref: str, requested_priority: str) -> dict:
    return _queue_lifecycle.bump_queue_command_job_locked(
        job_ref,
        requested_priority,
        queue_lock_path_fn=queue_lock_path,
        file_lock_fn=file_lock,
        load_queue_unlocked_fn=load_queue_unlocked,
        find_queue_command_job_unlocked_fn=_queue_orchestrator.find_queue_command_job_unlocked,
        set_pending_job_priority_unlocked_fn=lambda job, priority: _queue_orchestrator.set_pending_job_priority_unlocked(
            job,
            priority,
            now_iso_fn=now_iso,
        ),
        save_queue_unlocked_fn=save_queue_unlocked,
        summarize_job_fn=summarize_job,
    )


def cancel_queue_command_job(job_ref: str) -> dict:
    return _queue_lifecycle.cancel_queue_command_job_locked(
        job_ref,
        queue_lock_path_fn=queue_lock_path,
        file_lock_fn=file_lock,
        load_queue_unlocked_fn=load_queue_unlocked,
        find_queue_command_job_unlocked_fn=_queue_orchestrator.find_queue_command_job_unlocked,
        cancel_job_unlocked_fn=cancel_job_unlocked,
        trim_completed_jobs_fn=trim_completed_jobs,
        save_queue_unlocked_fn=save_queue_unlocked,
        summarize_job_fn=summarize_job,
    )


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
    return _queue_orchestrator.job_sort_key(job)


def queue_status_groups(queue: list[dict]) -> tuple[list[dict], list[dict], list[dict]]:
    return _queue_orchestrator.queue_status_groups(queue)


def recent_completed_jobs_for_status(completed_jobs: list[dict], *, limit: int = 5) -> list[dict]:
    return _queue_orchestrator.recent_completed_jobs_for_status(completed_jobs, limit=limit)


def reconcile_running_jobs_unlocked(queue: list[dict]) -> tuple[list[dict], bool]:
    return _queue_lifecycle.reconcile_running_jobs_unlocked(
        queue,
        stale_running_jobs_unlocked_fn=stale_running_jobs_unlocked,
        stale_running_reconciliation_actions_unlocked_fn=_queue_orchestrator.stale_running_reconciliation_actions_unlocked,
        supersede_job_unlocked_fn=supersede_job_unlocked,
        requeue_stale_running_job_unlocked_fn=lambda job: _queue_orchestrator.requeue_stale_running_job_unlocked(
            job,
            now_iso_fn=now_iso,
        ),
    )


def read_runner_info() -> dict | None:
    return _runner_state.read_runner_info()


def pid_alive(pid: int | None) -> bool:
    return _runner_state.pid_alive(pid)


def current_runner_info() -> dict | None:
    return _runner_state.current_runner_info()


def stale_running_jobs_unlocked(queue: list[dict]) -> list[dict]:
    return _runner_state.stale_running_jobs_for_current_runner(
        queue,
        stale_running_jobs_for_runner_unlocked_fn=_queue_orchestrator.stale_running_jobs_for_runner_unlocked,
    )


def update_job_target_state(job_id: str, target_name: str, **fields) -> None:
    _queue_lifecycle.update_job_target_state_locked(
        job_id,
        target_name,
        fields,
        queue_lock_path_fn=queue_lock_path,
        file_lock_fn=file_lock,
        load_queue_unlocked_fn=load_queue_unlocked,
        update_job_target_state_unlocked_fn=lambda queue, current_job_id, current_target_name, current_fields: _queue_orchestrator.update_job_target_state_unlocked(
            queue,
            current_job_id,
            current_target_name,
            current_fields,
            now_iso_fn=now_iso,
        ),
        save_queue_unlocked_fn=save_queue_unlocked,
    )


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
    return _queue_lifecycle.reclaim_stale_remote_validators_locked(
        queue_lock_path_fn=queue_lock_path,
        file_lock_fn=file_lock,
        load_queue_unlocked_fn=load_queue_unlocked,
        collect_stale_windows_cleanup_candidates_unlocked_fn=collect_stale_windows_cleanup_candidates_unlocked,
        save_queue_unlocked_fn=save_queue_unlocked,
        reclaim_stale_remote_validator_candidates_fn=_cleanup.reclaim_stale_remote_validator_candidates,
        cleanup_validator_fn=cleanup_stale_windows_validator,
        update_job_target_state_fn=update_job_target_state,
        now_fn=now_iso,
        trim_line_fn=trim_line,
    )


def write_runner_info(info: dict) -> None:
    _runner_state.write_runner_info(info)


def update_runner_active_targets(job_id: str, active_targets: dict | None) -> None:
    def update_info(info: dict, current_job_id: str, current_active_targets: dict | None) -> bool:
        return _queue_orchestrator.update_runner_info_active_targets(
            info,
            current_job_id,
            current_active_targets,
            now_iso_fn=now_iso,
        )

    _runner_state.update_current_runner_active_targets(
        job_id,
        active_targets,
        update_runner_info_active_targets_fn=update_info,
    )


def clear_runner_info() -> None:
    _runner_state.clear_runner_info()


def find_job_unlocked(queue: list[dict], job_ref: str, statuses: set[str] | None = None) -> dict | None:
    return _queue_orchestrator.find_job_unlocked(queue, job_ref, statuses)


def load_job(job_id: str) -> dict | None:
    return _queue_lifecycle.load_job_locked(
        job_id,
        queue_lock_path_fn=queue_lock_path,
        file_lock_fn=file_lock,
        load_queue_unlocked_fn=load_queue_unlocked,
        reconcile_running_jobs_unlocked_fn=reconcile_running_jobs_unlocked,
        save_queue_unlocked_fn=save_queue_unlocked,
        find_job_unlocked_fn=find_job_unlocked,
        normalize_job_fn=normalize_job,
    )


def claim_next_job() -> dict | None:
    return _queue_lifecycle.claim_next_job_locked(
        root=ROOT,
        queue_lock_path_fn=queue_lock_path,
        file_lock_fn=file_lock,
        load_queue_unlocked_fn=load_queue_unlocked,
        reconcile_running_jobs_unlocked_fn=reconcile_running_jobs_unlocked,
        save_queue_unlocked_fn=save_queue_unlocked,
        claim_next_job_unlocked_fn=lambda queue, *, runner: _queue_orchestrator.claim_next_job_unlocked(
            queue,
            runner=runner,
            now_iso_fn=now_iso,
        ),
        normalize_job_fn=normalize_job,
        pid_fn=os.getpid,
    )


def finalize_job(job_id: str, result: dict, result_path: Path) -> None:
    _queue_lifecycle.finalize_job_locked(
        job_id,
        result,
        result_path,
        queue_lock_path_fn=queue_lock_path,
        file_lock_fn=file_lock,
        load_queue_unlocked_fn=load_queue_unlocked,
        complete_job_unlocked_fn=lambda queue, current_job_id, current_result, current_result_path: _queue_orchestrator.complete_job_unlocked(
            queue,
            current_job_id,
            current_result,
            current_result_path,
            now_iso_fn=now_iso,
        ),
        trim_completed_jobs_with_removed_ids_fn=trim_completed_jobs_with_removed_ids,
        save_queue_unlocked_fn=save_queue_unlocked,
        collect_local_ci_cleanup_plan_fn=collect_local_ci_cleanup_plan,
        apply_local_ci_cleanup_plan_fn=apply_local_ci_cleanup_plan,
        keep_results=KEEP_COMPLETED_JOBS,
        keep_logs=KEEP_COMPLETED_JOBS,
        keep_bundles=0,
        include_prepared=False,
    )


def wait_for_job(job_id: str, config: dict) -> tuple[dict | None, int]:
    return _queue_lifecycle.wait_for_job_completion(
        job_id,
        config,
        load_job_fn=load_job,
        load_result_fn=load_result,
        drain_pending_jobs_fn=drain_pending_jobs,
        current_runner_info_fn=current_runner_info,
        sleep_fn=time.sleep,
        poll_secs=WAIT_POLL_SECS,
    )


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
    return _target_preflight.ssh_probe(
        host,
        timeout,
        run_ssh_subprocess_fn=run_ssh_subprocess,
    )


def ssh_reachable(host: str, timeout: int = 5) -> bool:
    return _target_preflight.ssh_reachable(
        host,
        timeout,
        ssh_probe_fn=ssh_probe,
    )


def ssh_failure_detail(host: str, timeout: int = 5) -> str:
    return _target_preflight.ssh_failure_detail(
        host,
        timeout,
        ssh_probe_fn=ssh_probe,
    )


def ssh_command_result(host: str, remote_cmd: str, *, timeout: int = 30) -> subprocess.CompletedProcess[str]:
    return _target_preflight.ssh_command_result(
        host,
        remote_cmd,
        timeout=timeout,
        run_ssh_subprocess_fn=run_ssh_subprocess,
    )


def utmctl_vm_status(vm_name: str) -> str | None:
    return _target_preflight.utmctl_vm_status(
        vm_name,
        run_fn=subprocess.run,
    )


def utmctl_start(vm_name: str) -> bool:
    return _target_preflight.utmctl_start(
        vm_name,
        run_fn=subprocess.run,
    )


def ensure_host_reachable(target_name: str, target_cfg: dict, defaults: dict) -> str | None:
    return _target_preflight.ensure_host_reachable(
        target_name,
        target_cfg,
        defaults,
        ssh_reachable_fn=ssh_reachable,
        utmctl_vm_status_fn=utmctl_vm_status,
        utmctl_start_fn=utmctl_start,
        time_fn=time.time,
        sleep_fn=time.sleep,
        print_fn=print,
    )


def config_source_name(path: Path) -> str:
    return _target_preflight.config_source_name(
        path,
        environ=os.environ,
        shared_config_path_fn=shared_config_path,
    )


def config_material_for_targets(config: dict, targets: list[str]) -> dict:
    return _target_preflight.config_material_for_targets(config, targets)


def find_material_config_drift(targets: list[str]) -> list[str]:
    return _target_preflight.find_material_config_drift(
        targets,
        shared_config_path_fn=shared_config_path,
        worktree_config_path_fn=worktree_config_path,
        config_material_for_targets_fn=config_material_for_targets,
    )


def preflight_target_host_state(target_name: str, target_cfg: dict, defaults: dict) -> dict:
    return _target_preflight.preflight_target_host_state(
        target_name,
        target_cfg,
        defaults,
        ssh_reachable_fn=ssh_reachable,
    )


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
    return _target_preflight.build_submission_metadata(
        config,
        branch,
        sha,
        targets,
        priority,
        validation,
        allow_root_mismatch=allow_root_mismatch,
        allow_unreachable_targets=allow_unreachable_targets,
        root=ROOT,
        cwd_fn=Path.cwd,
        git_root_for_fn=git_root_for,
        config_path_fn=config_path,
        config_source_name_fn=config_source_name,
        preflight_target_host_state_fn=preflight_target_host_state,
        find_material_config_drift_fn=find_material_config_drift,
        normalize_provenance_fn=normalize_provenance,
        environ=os.environ,
    )


def print_submission_metadata(metadata: dict) -> None:
    return _target_preflight.print_submission_metadata(
        metadata,
        short_sha_fn=short_sha,
        provenance_summary_fn=provenance_summary,
        print_fn=print,
    )


# ── Validation Runners ───────────────────────────────────────────────────────


def remote_commit_error(target_name: str, host: str, job: dict) -> str:
    return _execution.remote_commit_error(target_name, host, job)


def parse_progress_marker(line: str) -> dict:
    return _execution.parse_progress_marker(line)


def prepared_state_root(target_name: str, validation: str) -> Path:
    return _execution.prepared_state_root(target_name, validation)


def should_reuse_prepared_state(job: dict) -> bool:
    return _execution.should_reuse_prepared_state(job)


def local_validation_command(job: dict, exclude_tests: str = "") -> tuple[list[str], str]:
    return _execution.local_validation_command(job, exclude_tests)


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
    return _execution.posix_ssh_validation_command(
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
    return _execution.validation_result_from_run(
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
    return _execution.validation_error_result(
        target_name,
        detail,
        log_path=log_path,
        transport_mode=transport_mode,
    )


def unreachable_target_result(target_name: str, detail: str = "Host unreachable") -> dict:
    return _execution.unreachable_target_result(target_name, detail)


def target_exception_result(target_name: str, exc: Exception) -> dict:
    return _execution.target_exception_result(target_name, exc)


def completed_job_result(job: dict, results: list[dict]) -> dict:
    return _execution.completed_job_result(
        job,
        results,
        completed_at=now_iso(),
        provenance=normalize_provenance(job.get("provenance")),
    )


def sorted_target_results(results: list[dict]) -> list[dict]:
    return _execution.sorted_target_results(results)


def run_target_tasks(
    tasks: list[tuple[str, Callable[[], dict]]],
    *,
    on_target_complete: Callable[[str, dict], None],
) -> list[dict]:
    return _execution.run_target_tasks(
        tasks,
        exception_result_fn=target_exception_result,
        on_target_complete=on_target_complete,
    )


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
    return _execution.run_logged_command(
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
    return _execution.run_local_validation(
        job,
        exclude_tests,
        report_progress,
        root=ROOT,
        print_fn=print,
        short_sha_fn=short_sha,
        prepare_target_log_fn=prepare_target_log,
        now_iso_fn=now_iso,
        local_validation_command_fn=local_validation_command,
        run_logged_command_fn=run_logged_command,
        validation_result_from_run_fn=validation_result_from_run,
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
    return _execution.run_posix_ssh_validation(
        target_name,
        host,
        repo_path,
        job,
        exclude_tests,
        config,
        report_progress,
        print_fn=print,
        short_sha_fn=short_sha,
        prepare_target_log_fn=prepare_target_log,
        now_iso_fn=now_iso,
        sync_job_bundle_to_ssh_host_fn=sync_job_bundle_to_ssh_host,
        posix_ssh_validation_command_fn=posix_ssh_validation_command,
        run_logged_command_fn=run_logged_command,
        validation_result_from_run_fn=validation_result_from_run,
        validation_error_result_fn=validation_error_result,
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
    return _execution.windows_validation_script(
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
        ps_literal_fn=ps_literal,
    )


def validate_ci_branch_name(branch: str) -> str:
    return _queue_orchestrator.validate_ci_branch_name(branch)


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
    return _execution.run_windows_ssh_validation(
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
        root=ROOT,
        prepare_target_log_fn=prepare_target_log,
        sync_job_bundle_to_ssh_host_fn=sync_job_bundle_to_ssh_host,
        validation_error_result_fn=validation_error_result,
        ensure_windows_remote_repo_checkout_fn=ensure_windows_remote_repo_checkout,
        git_origin_clone_url_fn=git_origin_clone_url,
        print_fn=print,
        short_sha_fn=short_sha,
        now_iso_fn=now_iso,
        probe_windows_ssh_cmake_settings_fn=probe_windows_ssh_cmake_settings,
        windows_validation_script_fn=windows_validation_script,
        windows_ssh_powershell_command_fn=windows_ssh_powershell_command,
        run_logged_command_fn=run_logged_command,
        validation_result_from_run_fn=validation_result_from_run,
    )


# ── Job Processing ───────────────────────────────────────────────────────────


def config_for_job_execution(job: dict, config: dict) -> dict:
    return _execution.config_for_job_execution(
        job,
        config,
        load_config_file_fn=load_config_file,
        warn_fn=print,
    )


def submission_target_state(job: dict, target_name: str) -> dict:
    return _execution.submission_target_state(job, target_name)


def resolve_ssh_target_execution(job: dict, target_name: str, target_cfg: dict, defaults: dict) -> tuple[str | None, str | None]:
    return _execution.resolve_ssh_target_execution(
        job,
        target_name,
        target_cfg,
        defaults,
        ensure_host_reachable_fn=ensure_host_reachable,
    )


def _build_target_tasks(job: dict, config: dict, progress_factory=None) -> list[tuple[str, Callable[[], dict]]]:
    return _execution.build_target_tasks(
        job,
        config,
        enabled_targets_fn=enabled_targets,
        resolve_ssh_target_execution_fn=resolve_ssh_target_execution,
        run_local_validation_fn=run_local_validation,
        run_posix_ssh_validation_fn=run_posix_ssh_validation,
        run_windows_ssh_validation_fn=run_windows_ssh_validation,
        progress_factory=progress_factory,
    )


def process_job(job: dict, config: dict) -> dict:
    return _execution.process_job(
        job,
        config,
        print_fn=print,
        short_sha_fn=short_sha,
        config_for_job_execution_fn=config_for_job_execution,
        build_target_tasks_fn=_build_target_tasks,
        target_state_snapshot_fn=target_state_snapshot,
        update_runner_active_targets_fn=update_runner_active_targets,
        update_job_active_targets_fn=update_job_active_targets,
        updated_target_state_fn=updated_target_state,
        initial_target_state_fn=initial_target_state,
        completed_target_state_fn=completed_target_state,
        now_iso_fn=now_iso,
        run_target_tasks_fn=run_target_tasks,
        completed_job_result_fn=completed_job_result,
        sorted_target_results_fn=sorted_target_results,
    )


def save_result(result: dict) -> Path:
    ensure_state_dirs()
    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    branch_slug = result["branch"].replace("/", "-")
    path = results_dir() / f"{ts}-{result['job_id']}-{branch_slug}.json"
    path.write_text(json.dumps(result, indent=2) + "\n")
    update_evidence_index(result, path)
    return path


def print_result(result: dict, result_path: Path | None = None) -> None:
    result = normalize_result(result)
    print(f"\n--- Result: [{result['job_id']}] {result['branch']} ---")
    validation_line = result_validation_line(result)
    if validation_line:
        print(validation_line)
    print(result_execution_line(result))
    for line in result_target_lines(result):
        print(line)
    print(result_overall_line(result))
    if result_path:
        print(f"  Saved: {result_path}")
    print()


def drain_pending_jobs(config: dict, *, blocking: bool) -> tuple[bool, bool]:
    return _queue_lifecycle.drain_pending_jobs_locked(
        config,
        blocking=blocking,
        root=ROOT,
        drain_lock_path_fn=drain_lock_path,
        file_lock_fn=file_lock,
        lock_busy_error_cls=LockBusyError,
        write_runner_info_fn=write_runner_info,
        clear_runner_info_fn=clear_runner_info,
        reclaim_stale_remote_validators_fn=reclaim_stale_remote_validators,
        claim_next_job_fn=claim_next_job,
        process_job_fn=process_job,
        save_result_fn=save_result,
        finalize_job_fn=finalize_job,
        print_result_fn=print_result,
        now_fn=now_iso,
        pid_fn=os.getpid,
    )


# ── GitHub Helpers ───────────────────────────────────────────────────────────


def print_local_ci_state_footprint(*, indent: str = "") -> None:
    return _cleanup_cli.print_local_ci_state_footprint(
        local_ci_state_footprint_fn=local_ci_state_footprint,
        state_footprint_lines_fn=state_footprint_lines,
        indent=indent,
    )


def print_local_ci_cleanup_plan(plan: dict, *, dry_run: bool) -> None:
    return _cleanup_cli.print_local_ci_cleanup_plan(
        plan,
        dry_run=dry_run,
        cleanup_plan_lines_fn=cleanup_plan_lines,
    )


def cmd_cleanup(args: argparse.Namespace) -> int:
    return _cleanup_cli.cmd_cleanup(
        args,
        load_queue_fn=load_queue,
        collect_cleanup_plan_fn=collect_local_ci_cleanup_plan,
        apply_cleanup_plan_fn=apply_local_ci_cleanup_plan,
        print_cleanup_plan_fn=print_local_ci_cleanup_plan,
        print_state_footprint_fn=print_local_ci_state_footprint,
        format_size_fn=format_size_bytes,
        describe_path_fn=describe_path_for_cleanup,
    )


def resolve_submission_options(
    args: argparse.Namespace, command: str
) -> tuple[dict, str, str, list[str], str, str, dict]:
    config = load_config()
    branch = args.branch or current_branch()
    if args.sha:
        sha = args.sha
    elif args.branch:
        sha = resolve_git_ref_sha(branch)
    else:
        sha = current_sha()
    targets = resolve_targets(config, parse_targets_arg(getattr(args, "targets", None)))
    priority = normalize_priority(getattr(args, "priority", None) or default_priority_for(command, config))
    validation = normalize_validation_mode("smoke" if getattr(args, "smoke", False) else "full")
    submission = build_submission_metadata(
        config,
        branch,
        sha,
        targets,
        priority,
        validation,
        allow_root_mismatch=bool(getattr(args, "allow_root_mismatch", False)),
        allow_unreachable_targets=bool(getattr(args, "allow_unreachable_targets", False)),
    )
    return config, branch, sha, targets, priority, validation, submission


def cmd_enqueue(args: argparse.Namespace) -> int:
    try:
        _config, branch, sha, targets, priority, validation, submission = resolve_submission_options(args, "enqueue")
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    print_submission_metadata(submission)
    job, created = enqueue_job(branch, sha, priority, targets, "enqueue", validation, submission=submission)
    print(enqueue_command_result_line(job, created=created))
    return 0


def cmd_drain(_args: argparse.Namespace) -> int:
    try:
        config = load_config()
    except FileNotFoundError as exc:
        print(f"Error: {exc}")
        return 1

    acquired, any_failure = drain_pending_jobs(config, blocking=False)
    if not acquired:
        print(drain_runner_active_line(current_runner_info()))
        return 0

    notify("CI complete" + (" - PASSED" if not any_failure else " - FAILED"))
    return 1 if any_failure else 0


def cmd_run(args: argparse.Namespace) -> int:
    try:
        config, branch, sha, targets, priority, validation, submission = resolve_submission_options(args, "run")
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    print_submission_metadata(submission)

    # Auto-dispatch Namespace for unreachable targets
    failover_targets = submission.get("namespace_failover_targets", [])
    if failover_targets:
        ga_cfg = config.get("github_actions", {})
        repository = ga_cfg.get("repository", "danielraffel/pulp")
        print(f"\n⚠️  Namespace failover: dispatching {', '.join(failover_targets)} to Namespace")
        try:
            gh_workflow_dispatch(repository, "build.yml", branch, {"runner_provider": "namespace"})
            print(f"  Dispatched Namespace run for {branch}")
        except Exception as exc:
            print(f"  Warning: Namespace dispatch failed: {exc}")

    # Only run local targets (skip unreachable ones that were dispatched to Namespace)
    local_targets = [t for t in targets if t not in failover_targets]
    if local_targets:
        job, created = enqueue_job(branch, sha, priority, local_targets, "run", validation, submission=submission)
        print(enqueue_command_result_line(job, created=created))

        result, exit_code = wait_for_job(job["id"], config)
        if result is not None:
            print_result(result, Path(load_job(job["id"])["result_file"]))
    else:
        print("All targets dispatched to Namespace — no local work to do.")
        exit_code = 0

    if failover_targets:
        print(f"\nNote: {', '.join(failover_targets)} results are on Namespace.")
        print(f"  Check with: python3 tools/local-ci/local_ci.py cloud status")

    notify("CI run complete" + (" - PASSED" if exit_code == 0 else " - FAILED"))
    return exit_code


def cmd_ship(args: argparse.Namespace) -> int:
    try:
        config, branch, sha, targets, priority, validation, submission = resolve_submission_options(args, "ship")
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1
    if validation != "full":
        print("Error: ship only supports full validation. Use `run --smoke` or `check --smoke` for preflight.")
        return 1

    base = args.base or "main"
    if branch == base:
        print(f"Error: cannot ship {base} to itself. Checkout a feature branch first.")
        return 1

    if not gh_available():
        print("Error: gh CLI not available or not authenticated. Run: gh auth login")
        return 1

    print(f"\n=== Shipping {branch} -> {base} ===\n")
    print_submission_metadata(submission)
    print(f"  Pushing {branch}...")
    push = subprocess.run(
        ["git", "push", "-u", "origin", branch],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if push.returncode != 0:
        print(f"  Push failed: {push.stderr.strip()}")
        return 1

    print("  Creating PR...")
    pr_number = gh_pr_create(branch, base)
    if pr_number is None:
        print("  Failed to create or find PR.")
        return 1
    print(f"  PR #{pr_number} ready")

    job, _created = enqueue_job(branch, sha, priority, targets, "ship", validation, submission=submission)
    print(f"  Queueing CI: {summarize_job(job)}")
    result, exit_code = wait_for_job(job["id"], config)
    if result is None:
        return 1

    gh_pr_comment(pr_number, format_ci_comment(result))
    if result["overall"] == "pass":
        print(f"  All targets passed. Merging PR #{pr_number}...")
        if gh_pr_merge(pr_number):
            print(f"  PR #{pr_number} merged and branch deleted.")
            notify(f"PR #{pr_number} shipped to {base}!")
            return 0
        print(f"  Merge failed. PR #{pr_number} is still open.")
        notify(f"PR #{pr_number} CI passed but merge failed")
        return 1

    print(f"  CI failed. PR #{pr_number} left open for review.")
    notify(f"PR #{pr_number} CI failed")
    return exit_code


def cmd_check(args: argparse.Namespace) -> int:
    if not gh_available():
        print("Error: gh CLI not available. Run: gh auth login")
        return 1

    pr_info = gh_pr_head(args.pr)
    if pr_info is None:
        return 1

    pr_number, branch, sha = pr_info
    print(f"  PR #{pr_number} -> branch: {branch} @ {short_sha(sha)}")

    try:
        config = load_config()
        targets = resolve_targets(config, parse_targets_arg(args.targets))
        priority = normalize_priority(args.priority or default_priority_for("check", config))
        validation = normalize_validation_mode("smoke" if getattr(args, "smoke", False) else "full")
        submission = build_submission_metadata(
            config,
            branch,
            sha,
            targets,
            priority,
            validation,
            allow_root_mismatch=bool(getattr(args, "allow_root_mismatch", False)),
            allow_unreachable_targets=bool(getattr(args, "allow_unreachable_targets", False)),
        )
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    print_submission_metadata(submission)
    job, _created = enqueue_job(branch, sha, priority, targets, "check", validation, submission=submission)
    print(f"  Queueing CI: {summarize_job(job)}")
    result, exit_code = wait_for_job(job["id"], config)
    if result is None:
        return 1

    gh_pr_comment(pr_number, format_ci_comment(result))
    notify("CI check complete" + (" - PASSED" if exit_code == 0 else " - FAILED"))
    return exit_code


def cmd_bump(args: argparse.Namespace) -> int:
    try:
        requested_priority = normalize_priority(args.priority)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    try:
        result = bump_queue_command_job(args.job, requested_priority)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    exit_code, line = bump_queue_command_result_line(result, args.job)
    print(line)
    return exit_code


def cmd_cancel(args: argparse.Namespace) -> int:
    try:
        result = cancel_queue_command_job(args.job)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    exit_code, line = cancel_queue_command_result_line(result, args.job)
    print(line)
    return exit_code


def cmd_list(_args: argparse.Namespace) -> int:
    if not gh_available():
        print("Error: gh CLI not available. Run: gh auth login")
        return 1

    prs = gh_pr_list_open()
    for line in open_pr_list_lines(prs):
        print(line)
    return 0


def resolve_job_for_logs(job_ref: str | None) -> dict | None:
    return _queue_orchestrator.select_job_for_logs(
        load_queue(),
        current_runner_info(),
        job_ref,
    )


def cmd_logs(args: argparse.Namespace) -> int:
    try:
        job = resolve_job_for_logs(args.job)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    if job is None:
        print(missing_job_logs_line())
        return 1

    paths: list[Path]
    if args.target:
        path = target_log_path(job["id"], args.target)
        paths = [path]
    else:
        log_dir = job_logs_dir(job["id"])
        paths = sorted(log_dir.glob("*.log"))

    if not paths:
        print(missing_log_files_line(job))
        return 1

    print(f"{job_logs_header_line(job)}\n")
    for path in paths:
        print(log_section_header_line(path.stem))
        lines = tail_lines(path, args.lines)
        if lines:
            print("".join(lines).rstrip())
        else:
            print(empty_log_line())
        print()
    return 0


def cmd_evidence(args: argparse.Namespace) -> int:
    branch = args.branch or current_branch()
    header = evidence_scope_header_line(branch, args.sha)
    if header:
        print(header)

    found = print_evidence_summary(branch=branch, sha=args.sha, limit=args.limit)
    if not found:
        print(evidence_empty_line(has_header=header is not None))
        return 1
    return 0


def cmd_status(_args: argparse.Namespace) -> int:
    try:
        config = load_config()
    except FileNotFoundError as exc:
        print(f"Error: {exc}")
        return 1

    queue = load_queue()
    pending, running, completed = queue_status_groups(queue)
    runner = current_runner_info()

    print(f"State: {state_dir()}")
    print(f"Config: {config_path()}")

    print(f"\n{status_runner_line(runner)}")

    if running:
        print(f"\nRunning ({len(running)}):")
        for job in running:
            print(f"  {summarize_job(job)} started {job.get('started_at', '?')}")
            for line in status_submission_lines(job):
                print(f"    {line}")
            active_targets = status_active_targets(job, runner)
            target_summary = summarize_active_targets(active_targets, job.get("targets"))
            if target_summary:
                print(f"    live targets: {target_summary}")
            for line in status_target_detail_lines(job, active_targets):
                print(f"    {line}")
    else:
        print("\nNo running jobs.")

    if pending:
        print(f"\nPending ({len(pending)}):")
        for job in pending:
            print(f"  {summarize_job(job)} queued {job.get('queued_at', '?')}")
            for line in status_submission_lines(job):
                print(f"    {line}")
            active_targets = status_active_targets(job)
            target_summary = summarize_active_targets(active_targets, job.get("targets"))
            if target_summary:
                progress_at = job.get("last_progress_at") or job.get("requeued_at") or "?"
                print(f"    last known targets: {target_summary} (updated {progress_at})")
            for line in status_target_detail_lines(job, active_targets):
                print(f"    {line}")
    else:
        print("\nNo pending jobs.")

    recent_completed = recent_completed_jobs_for_status(completed)
    if recent_completed:
        print(f"\nRecent ({len(recent_completed)}):")
        for job in recent_completed:
            result_file = job.get("result_file")
            if result_file and Path(result_file).exists():
                result = load_result(Path(result_file))
                print(f"  {recent_completed_status_line(job, result)}")
            else:
                print(f"  {recent_completed_missing_result_line(job)}")

    branch = current_branch()
    if branch:
        print(f"\nEvidence ({branch}):")
        if not print_evidence_summary(branch=branch, limit=2, indent="  "):
            print("  (none)")

    cloud_records = list_cloud_records(limit=5)
    all_cloud_records = list_cloud_records(limit=None)
    cloud_config = load_optional_config()
    cloud_settings_note = ""
    cloud_settings = github_actions_settings_for_display(cloud_config)
    try:
        resolved_cloud_settings = resolve_github_actions_settings(cloud_config)
        cloud_settings = resolved_cloud_settings
    except ValueError as exc:
        cloud_settings_note = str(exc)
    default_workflow_key = cloud_settings.get("workflow", "build")
    try:
        default_provider, _default_provider_source = resolve_default_provider_for_workflow(
            cloud_settings,
            default_workflow_key,
        )
    except ValueError:
        default_provider = cloud_settings.get("provider", "github-hosted")

    print(
        f"\nCloud defaults: workflow={default_workflow_key} provider={default_provider} "
        "(`pulp ci-local cloud defaults` for selectors and sources)"
    )
    if cloud_settings_note:
        print(f"  note: {cloud_settings_note}")

    if cloud_records:
        print_billing_period_summary(estimate_billing_period_totals(all_cloud_records, cloud_config), indent="  ")
        print("\nCloud (latest 5 known to this machine):")
        for record in cloud_records:
            print(f"  {cloud_record_summary(record, cloud_config)}")

    print()
    print_local_ci_state_footprint(indent="  ")

    print("\nVM Status:")
    for vm_name in ["Ubuntu 24.04 desktop", "Windows"]:
        print(f"  {vm_name}: {utmctl_vm_status(vm_name) or 'not found'}")

    for host in [target_cfg.get("host") for target_cfg in config.get("targets", {}).values() if target_cfg.get("type") == "ssh"]:
        if host:
            print(f"  ssh {host}: {'up' if ssh_reachable(host, 3) else 'down'}")

    return 0


def cmd_desktop_install(args: argparse.Namespace) -> int:
    try:
        config = load_config()
        target = resolve_desktop_target(config, args.target)
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    artifact_root = Path(config["desktop_automation"]["artifact_root"])
    ok, detail = _check_writable_dir(artifact_root)
    if not ok:
        print(f"Error: desktop artifact root is not writable: {detail}")
        return 1

    contract = desktop_target_contract(args.target, target)
    remote_bootstrap_ready = target["target_type"] != "ssh"
    remote_tooling_ready = target["target_type"] != "ssh"
    remote_repo_checkout_ready = target["target_type"] != "ssh"
    tooling_probe = None
    tooling_installed: list[str] = []
    repo_checkout_probe = None
    if target["target_type"] == "ssh" and target["adapter"] == "windows-session-agent":
        host = ensure_host_reachable(args.target, target, config.get("defaults", {}))
        if host:
            try:
                bootstrap_result = bootstrap_windows_session_agent(host, contract)
                probe = probe_windows_session_agent(host, contract)
                remote_bootstrap_ready = bool(
                    probe.get("task_present")
                    and probe.get("agent_root_exists")
                    and probe.get("jobs_dir_exists")
                    and probe.get("results_dir_exists")
                    and probe.get("script_exists")
                )
                contract = {
                    **contract,
                    "remote_root": bootstrap_result.get("remote_root", contract.get("remote_root")),
                    "script_path": bootstrap_result.get("script_path", contract.get("script_path")),
                }
                install_bundle_sha = subprocess.run(
                    ["git", "rev-parse", "HEAD"],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    check=True,
                ).stdout.strip()
                install_bundle_job = {"id": uuid.uuid4().hex[:12], "sha": install_bundle_sha}
                install_bundle_name, install_bundle_ref = sync_job_bundle_to_ssh_host(host, install_bundle_job)
                tooling_result = ensure_windows_remote_tooling(host)
                tooling_probe = tooling_result["probe"]
                tooling_installed = tooling_result["installed"]
                remote_tooling_ready = windows_remote_tooling_ready(tooling_probe)
                repo_checkout_probe = ensure_windows_remote_repo_checkout(
                    host,
                    target.get("repo_path"),
                    remote_url=git_origin_clone_url(ROOT),
                    bundle_name=install_bundle_name,
                    bundle_ref=install_bundle_ref,
                )
                remote_repo_checkout_ready = windows_repo_checkout_ready(repo_checkout_probe)
                effective_repo_path = repo_checkout_probe.get("repo_path")
                if effective_repo_path and effective_repo_path != target.get("repo_path"):
                    update_target_repo_path(config, args.target, effective_repo_path)
                    save_config(config)
                    target = resolve_desktop_target(config, args.target)
            except (subprocess.SubprocessError, OSError, RuntimeError) as exc:
                remote_bootstrap_ready = False
                remote_tooling_ready = False
                remote_repo_checkout_ready = False
                print(f"Warning: remote bootstrap did not complete for `{args.target}`: {exc}")
        else:
            remote_bootstrap_ready = False
            remote_tooling_ready = False
            remote_repo_checkout_ready = False

    receipt = {
        "target": args.target,
        "adapter": target["adapter"],
        "bootstrap": target["bootstrap"],
        "target_type": target["target_type"],
        "host": target.get("host"),
        "repo_path": target.get("repo_path"),
        "artifact_root": str(artifact_root),
        "capability_tier": target.get("capability_tier", "v1"),
        "installed_at": now_iso(),
        "remote_bootstrap_ready": remote_bootstrap_ready,
        "remote_tooling_ready": remote_tooling_ready,
        "remote_repo_checkout_ready": remote_repo_checkout_ready,
        "tooling_probe": tooling_probe,
        "repo_checkout_probe": repo_checkout_probe,
        "contract": contract,
    }
    atomic_write_text(
        desktop_target_receipt_path(args.target),
        json.dumps(receipt, indent=2) + "\n",
    )

    print(f"Desktop target `{args.target}` prepared.")
    print(f"  adapter: {target['adapter']}")
    print(f"  bootstrap: {target['bootstrap']}")
    print(f"  artifact_root: {artifact_root}")
    if target["target_type"] == "ssh":
        if remote_bootstrap_ready:
            print("  remote bootstrap: ready")
        else:
            print("  remote bootstrap: pending; target profile recorded locally")
        if target["adapter"] == "windows-session-agent":
            if remote_tooling_ready:
                git_detail = windows_tooling_detail(tooling_probe or {}, "git") if tooling_probe else "git ready"
                print(f"  remote tooling: ready ({git_detail})")
            else:
                print("  remote tooling: pending; run `pulp ci-local desktop doctor windows` for remediation")
            if tooling_installed:
                print(f"  remote tooling installed: {', '.join(tooling_installed)}")
            if repo_checkout_probe and repo_checkout_probe.get("repo_path"):
                print(f"  remote repo checkout: {repo_checkout_probe['repo_path']}")
        if contract.get("task_name"):
            print(f"  task_name: {contract['task_name']}")
        if contract.get("remote_root"):
            print(f"  remote_root: {contract['remote_root']}")
    else:
        print("  remote bootstrap: not required for local target")
    return 0


def cmd_desktop_doctor(args: argparse.Namespace) -> int:
    try:
        config = load_config()
        target = resolve_desktop_target(config, args.target)
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    checks = desktop_doctor_checks(config, args.target)
    all_ok = True
    for check in checks:
        if check.get("required", True):
            all_ok = all_ok and check["ok"]
    if getattr(args, "json", False):
        payload = {
            "target": args.target,
            "adapter": target["adapter"],
            "bootstrap": target["bootstrap"],
            "ok": all_ok,
            "checks": checks,
        }
        print(json.dumps(payload, indent=2))
        return 0 if all_ok else 1
    print(f"Desktop doctor for `{args.target}`")
    print(f"  adapter: {target['adapter']}")
    print(f"  bootstrap: {target['bootstrap']}")
    for check in checks:
        if check["ok"]:
            status = "PASS"
        elif not check.get("required", True):
            status = "WARN"
        else:
            status = "FAIL"
        print(f"  {status:4s}  {check['name']}: {check['detail']}")
    return 0 if all_ok else 1


def cmd_desktop_status(args: argparse.Namespace) -> int:
    try:
        config = load_config()
    except FileNotFoundError as exc:
        print(f"Error: {exc}")
        return 1

    desktop_cfg = config["desktop_automation"]
    targets = desktop_cfg.get("targets", {})
    if args.target:
        if args.target not in targets:
            print(f"\nError: unknown desktop target `{args.target}`")
            return 1
        target_names = [args.target]
    else:
        target_names = sorted(targets)

    target_payloads: list[dict] = []
    for name in target_names:
        target = targets[name]
        receipt = desktop_receipt_for(name)
        capabilities = ", ".join(
            desktop_capabilities_for(target["adapter"], target["capability_tier"], target.get("optional"))
        )
        optional_capabilities = desktop_optional_capabilities(target.get("optional"))
        latest = desktop_run_manifests(config, target_name=name)[:1]
        latest_manifest = latest[0] if latest else None
        latest_run = desktop_run_summary(config, latest_manifest) if latest_manifest else None
        latest_proof_matches = desktop_proof_summaries(config, target_name=name, limit=1)
        latest_proof = latest_proof_matches[0] if latest_proof_matches else None
        target_info = {
            "name": name,
            "enabled": target["enabled"],
            "adapter": target["adapter"],
            "bootstrap": target["bootstrap"],
            "type": target["target_type"],
            "host": target.get("host"),
            "repo_path": target.get("repo_path"),
            "capability_tier": target["capability_tier"],
            "capabilities": desktop_capabilities_for(target["adapter"], target["capability_tier"], target.get("optional")),
            "capabilities_text": capabilities,
            "optional_features": normalize_desktop_optional_config(target.get("optional")),
            "optional_capabilities": optional_capabilities,
            "installed": bool(receipt),
            "installed_at": receipt.get("installed_at", "?") if receipt else None,
            "contract": receipt.get("contract") if receipt else desktop_target_contract(name, target),
            "remote_bootstrap_ready": receipt.get("remote_bootstrap_ready") if receipt else None,
            "remote_tooling_ready": receipt.get("remote_tooling_ready") if receipt else None,
            "remote_repo_checkout_ready": receipt.get("remote_repo_checkout_ready") if receipt else None,
            "tooling_probe": receipt.get("tooling_probe") if receipt else None,
            "repo_checkout_probe": receipt.get("repo_checkout_probe") if receipt else None,
            "latest_run": None,
            "latest_proof": latest_proof,
        }
        if latest_run:
            target_info["latest_run"] = {
                "label": latest_run["label"],
                "completed_at": latest_run["completed_at"],
                "interaction_mode": latest_run["interaction_mode"],
                "run_status": latest_run["run_status"],
                "source_mode": latest_run["source"]["mode"],
                "source_branch": latest_run["source"]["branch"],
                "source_sha": latest_run["source"]["sha"],
                "proof_scope": latest_run["proof_scope"],
                "host": latest_run["host"],
                "screenshot": latest_run["artifacts"]["screenshot"],
                "before_screenshot": latest_run["artifacts"]["before_screenshot"],
                "diff_screenshot": latest_run["artifacts"]["diff_screenshot"],
                "image_change": latest_run["artifacts"]["image_change"],
                "ui_snapshot": latest_run["artifacts"]["ui_snapshot"],
                "bundle_dir": latest_run["artifacts"]["bundle_dir"],
            }
        target_payloads.append(target_info)
    if getattr(args, "json", False):
        latest_publish_matches = desktop_publish_reports(config, limit=1)
        latest_publish = latest_publish_matches[0] if latest_publish_matches else None
        payload = {
            "artifact_root": desktop_cfg["artifact_root"],
            "publish_mode": desktop_cfg["publish_mode"],
            "publish_branch": desktop_cfg["publish_branch"],
            "retention_days": desktop_cfg["retention_days"],
            "latest_publish": latest_publish,
            "targets": target_payloads,
        }
        print(json.dumps(payload, indent=2))
        return 0

    latest_publish_matches = desktop_publish_reports(config, limit=1)
    latest_publish = latest_publish_matches[0] if latest_publish_matches else None
    for line in _desktop_cli.desktop_status_lines(
        desktop_cfg,
        target_payloads,
        latest_publish=latest_publish,
        short_sha_fn=short_sha,
        windows_tooling_detail_fn=windows_tooling_detail,
        windows_repo_checkout_detail_fn=windows_repo_checkout_detail,
    ):
        print(line)
    return 0


def cmd_desktop_config_show(args: argparse.Namespace) -> int:
    try:
        config = load_config()
    except FileNotFoundError as exc:
        print(f"Error: {exc}")
        return 1

    desktop_cfg = config["desktop_automation"]
    if getattr(args, "json", False):
        print(json.dumps(desktop_cfg, indent=2))
        return 0

    for line in _desktop_cli.desktop_config_show_lines(desktop_cfg):
        print(line)
    return 0


def cmd_desktop_config_set(args: argparse.Namespace) -> int:
    try:
        config = load_config()
    except FileNotFoundError as exc:
        print(f"Error: {exc}")
        return 1

    desktop_cfg = config.setdefault("desktop_automation", {})
    key = args.key
    raw_value = args.value
    payload_value = None
    try:
        if key == "artifact_root":
            desktop_cfg["artifact_root"] = raw_value
            payload_value = desktop_cfg["artifact_root"]
        elif key == "publish_mode":
            desktop_cfg["publish_mode"] = normalize_publish_mode(raw_value)
            payload_value = desktop_cfg["publish_mode"]
        elif key == "publish_branch":
            desktop_cfg["publish_branch"] = raw_value
            payload_value = desktop_cfg["publish_branch"]
        elif key == "retention_days":
            retention_days = int(raw_value)
            if retention_days < 0:
                raise ValueError("retention_days must be >= 0")
            desktop_cfg["retention_days"] = retention_days
            payload_value = desktop_cfg["retention_days"]
        elif key.startswith("target."):
            parts = key.split(".")
            if len(parts) != 3:
                raise ValueError("Target desktop config keys must look like target.<name>.<field>.")
            _, target_name, field = parts
            target_cfg = desktop_cfg.setdefault("targets", {}).setdefault(target_name, {})
            optional_cfg = dict(target_cfg.get("optional", {}))
            if field in {"webview_driver", "debug_attach", "video_capture", "frame_stats"}:
                optional_cfg[field] = parse_config_bool(raw_value)
            elif field in {"webdriver_url", "debugger_command"}:
                optional_cfg[field] = raw_value
            else:
                raise ValueError(
                    "Unsupported target desktop config field. Use one of: "
                    "target.<name>.webview_driver, target.<name>.webdriver_url, "
                    "target.<name>.debug_attach, target.<name>.debugger_command, "
                    "target.<name>.video_capture, target.<name>.frame_stats."
                )
            target_cfg["optional"] = optional_cfg
            payload_value = optional_cfg[field]
        else:
            raise ValueError(
                f"Unsupported desktop config key `{key}`. Use one of: artifact_root, publish_mode, publish_branch, retention_days, or target.<name>.<field>."
            )
        normalized = normalize_desktop_config(config)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    save_config(normalized)
    if key.startswith("target."):
        _, target_name, field = key.split(".")
        payload_value = normalized["desktop_automation"]["targets"][target_name]["optional"][field]
    else:
        payload_value = normalized["desktop_automation"][key]
    payload = {
        "key": key,
        "value": payload_value,
        "config_path": str(config_path()),
    }
    if getattr(args, "json", False):
        print(json.dumps(payload, indent=2))
        return 0

    for line in _desktop_cli.desktop_config_update_lines(payload):
        print(line)
    return 0


def cmd_desktop_config(args: argparse.Namespace) -> int:
    commands = {
        "show": cmd_desktop_config_show,
        "set": cmd_desktop_config_set,
    }
    handler = commands.get(args.desktop_config_command)
    if handler is None:
        print("Error: desktop config subcommand required (show, set)")
        return 1
    return handler(args)


def cmd_desktop_recent(args: argparse.Namespace) -> int:
    try:
        config = load_config()
    except FileNotFoundError as exc:
        print(f"Error: {exc}")
        return 1

    manifests = desktop_run_manifests(config, target_name=args.target, action=args.action)
    if not manifests:
        print("No desktop automation runs found.")
        return 0
    manifests = manifests[: args.limit]
    if getattr(args, "json", False):
        print(json.dumps({"runs": manifests}, indent=2))
        return 0

    run_summaries = [desktop_run_summary(config, manifest) for manifest in manifests]
    for line in _desktop_cli.desktop_recent_lines(run_summaries, short_sha_fn=short_sha):
        print(line)
    return 0


def cmd_desktop_proof(args: argparse.Namespace) -> int:
    try:
        config = load_config()
    except FileNotFoundError as exc:
        print(f"Error: {exc}")
        return 1

    try:
        proofs = desktop_proof_summaries(
            config,
            target_name=args.target,
            action=args.action,
            source_mode=args.source_mode,
            sha=args.sha,
            branch=args.branch,
            limit=args.limit,
        )
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print(json.dumps({"proofs": proofs}, indent=2))
        return 0

    if not proofs:
        print(
            _desktop_cli.desktop_proof_empty_line(
                target=args.target,
                action=args.action,
                source_mode=args.source_mode,
                sha=args.sha,
                branch=args.branch,
                short_sha_fn=short_sha,
            )
        )
        return 0

    for line in _desktop_cli.desktop_proof_lines(proofs, short_sha_fn=short_sha):
        print(line)
    return 0


def cmd_desktop_publish(args: argparse.Namespace) -> int:
    try:
        config = load_config()
    except FileNotFoundError as exc:
        print(f"Error: {exc}")
        return 1

    manifests = desktop_run_manifests(config, target_name=args.target, action=args.action)
    if not manifests:
        print("No desktop automation runs found.")
        return 0

    manifests = manifests[: args.limit]
    output_dir = Path(args.output).expanduser() if args.output else None
    try:
        report = stage_desktop_publish_report(config, manifests, output_dir=output_dir, label=args.label)
    except Exception as exc:
        print(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print(json.dumps(report, indent=2))
        return 0

    for line in _desktop_cli.desktop_publish_lines(report):
        print(line)
    return 0


def cmd_desktop_cleanup(args: argparse.Namespace) -> int:
    try:
        config = load_config()
    except FileNotFoundError as exc:
        print(f"Error: {exc}")
        return 1

    older_than = args.older_than_days if args.older_than_days is not None else config["desktop_automation"]["retention_days"]
    paths = prune_desktop_run_manifests(
        config,
        target_name=args.target,
        older_than_days=older_than,
        keep_last=args.keep_last,
    )
    if not paths:
        print(_desktop_cli.desktop_cleanup_empty_line())
        return 0

    for path in paths:
        shutil.rmtree(path, ignore_errors=False)

    write_desktop_run_rollups(config, target_name=args.target if args.target else None)
    if args.target is not None:
        write_desktop_run_rollups(config)

    if getattr(args, "json", False):
        print(json.dumps({"removed": [str(path) for path in paths]}, indent=2))
        return 0

    for line in _desktop_cli.desktop_cleanup_lines(paths):
        print(line)
    return 0


def windows_requires_pulp_app_selectors(args: argparse.Namespace) -> bool:
    return any([args.click_view_id, args.click_view_type, args.click_view_text, args.click_view_label])


def cmd_desktop_smoke(args: argparse.Namespace) -> int:
    try:
        config = load_config()
        target = resolve_desktop_target(config, args.target)
        source_request = make_desktop_source_request(args)
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    adapter = target["adapter"]
    if adapter == "macos-local":
        if sys.platform != "darwin":
            print(f"Error: macOS local desktop smoke must run on macOS (current platform: {sys.platform}).")
            return 1
        if not args.launch_command and not args.bundle_id:
            print("Error: desktop smoke requires either --command or --bundle-id.")
            return 1
        runner = lambda: run_macos_local_smoke(
            config,
            args.launch_command,
            action_name="smoke",
            bundle_id=args.bundle_id,
            label=args.label,
            output_path=args.output,
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            pulp_app_automation=getattr(args, "pulp_app_automation", False),
            capture_before=args.capture_before,
            settle_secs=args.settle_secs,
            timeout_secs=args.timeout,
            source_request=source_request,
        )
    elif adapter == "linux-xvfb":
        if args.bundle_id:
            print("Error: linux-xvfb desktop smoke currently supports --command only.")
            return 1
        if not args.launch_command:
            print("Error: desktop smoke requires --command for linux-xvfb targets.")
            return 1
        runner = lambda: run_linux_xvfb_remote_action(
            config,
            args.target,
            target,
            args.launch_command,
            action_name="smoke",
            label=args.label,
            output_path=args.output,
            pulp_app_automation=getattr(args, "pulp_app_automation", False),
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            capture_before=args.capture_before,
            settle_secs=args.settle_secs,
            timeout_secs=args.timeout,
            source_request=source_request,
        )
    elif adapter == "windows-session-agent":
        if args.bundle_id:
            print("Error: windows desktop smoke currently supports --command only.")
            return 1
        if not args.launch_command:
            print("Error: desktop smoke requires --command for windows targets.")
            return 1
        pulp_app_automation = bool(getattr(args, "pulp_app_automation", False))
        if args.capture_ui_snapshot and not pulp_app_automation:
            print("Error: windows desktop smoke currently supports --capture-ui-snapshot only with --pulp-app-automation.")
            return 1
        if windows_requires_pulp_app_selectors(args) and not pulp_app_automation:
            print("Error: windows desktop smoke currently supports view-target selectors only with --pulp-app-automation.")
            return 1
        runner = lambda: run_windows_session_agent_action(
            config,
            args.target,
            target,
            args.launch_command,
            action_name="smoke",
            label=args.label,
            output_path=args.output,
            pulp_app_automation=pulp_app_automation,
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            capture_before=args.capture_before,
            settle_secs=args.settle_secs,
            timeout_secs=args.timeout,
            source_request=source_request,
        )
    else:
        print(f"Error: desktop smoke is not implemented for `{args.target}` yet; adapter `{adapter}` is still pending.")
        return 1

    try:
        manifest = runner()
    except Exception as exc:
        print(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print(json.dumps(manifest, indent=2))
        return 0

    for line in _desktop_cli.desktop_action_success_lines("smoke", args.target, manifest):
        print(line)
    return 0


def cmd_desktop_click(args: argparse.Namespace) -> int:
    try:
        config = load_config()
        target = resolve_desktop_target(config, args.target)
        source_request = make_desktop_source_request(args)
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    adapter = target["adapter"]
    if adapter == "macos-local":
        if sys.platform != "darwin":
            print(f"Error: macOS local desktop click must run on macOS (current platform: {sys.platform}).")
            return 1
        if bool(args.launch_command) == bool(args.bundle_id):
            print("Error: desktop click requires exactly one of --command or --bundle-id.")
            return 1
        runner = lambda: run_macos_local_smoke(
            config,
            args.launch_command,
            action_name="click",
            bundle_id=args.bundle_id,
            label=args.label,
            output_path=args.output,
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            pulp_app_automation=getattr(args, "pulp_app_automation", False),
            capture_before=True,
            settle_secs=args.settle_secs,
            timeout_secs=args.timeout,
            source_request=source_request,
        )
    elif adapter == "linux-xvfb":
        if args.bundle_id:
            print("Error: linux-xvfb desktop click currently supports --command only.")
            return 1
        if not args.launch_command:
            print("Error: desktop click requires --command for linux-xvfb targets.")
            return 1
        runner = lambda: run_linux_xvfb_remote_action(
            config,
            args.target,
            target,
            args.launch_command,
            action_name="click",
            label=args.label,
            output_path=args.output,
            pulp_app_automation=getattr(args, "pulp_app_automation", False),
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            capture_before=True,
            settle_secs=args.settle_secs,
            timeout_secs=args.timeout,
            source_request=source_request,
        )
    elif adapter == "windows-session-agent":
        if args.bundle_id:
            print("Error: windows desktop click currently supports --command only.")
            return 1
        if not args.launch_command:
            print("Error: desktop click requires --command for windows targets.")
            return 1
        pulp_app_automation = bool(getattr(args, "pulp_app_automation", False))
        if args.capture_ui_snapshot and not pulp_app_automation:
            print("Error: windows desktop click currently supports --capture-ui-snapshot only with --pulp-app-automation.")
            return 1
        if windows_requires_pulp_app_selectors(args) and not pulp_app_automation:
            print("Error: windows desktop click currently supports view-target selectors only with --pulp-app-automation.")
            return 1
        runner = lambda: run_windows_session_agent_action(
            config,
            args.target,
            target,
            args.launch_command,
            action_name="click",
            label=args.label,
            output_path=args.output,
            pulp_app_automation=pulp_app_automation,
            capture_ui_snapshot=args.capture_ui_snapshot,
            click_point=args.click,
            click_view_id=args.click_view_id,
            click_view_type=args.click_view_type,
            click_view_text=args.click_view_text,
            click_view_label=args.click_view_label,
            capture_before=True,
            settle_secs=args.settle_secs,
            timeout_secs=args.timeout,
            source_request=source_request,
        )
    else:
        print(f"Error: desktop click is not implemented for `{args.target}` yet; adapter `{adapter}` is still pending.")
        return 1
    if not any([args.click, args.click_view_id, args.click_view_type, args.click_view_text, args.click_view_label]):
        print("Error: desktop click requires --click or one view-target selector.")
        return 1

    try:
        manifest = runner()
    except Exception as exc:
        print(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print(json.dumps(manifest, indent=2))
        return 0

    for line in _desktop_cli.desktop_action_success_lines("click", args.target, manifest):
        print(line)
    return 0


def cmd_desktop_inspect(args: argparse.Namespace) -> int:
    try:
        config = load_config()
        target = resolve_desktop_target(config, args.target)
        source_request = make_desktop_source_request(args)
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    adapter = target["adapter"]
    if adapter == "macos-local":
        if sys.platform != "darwin":
            print(f"Error: macOS local desktop inspect must run on macOS (current platform: {sys.platform}).")
            return 1
        if bool(args.launch_command) == bool(args.bundle_id):
            print("Error: desktop inspect requires exactly one of --command or --bundle-id.")
            return 1
        capture_ui_snapshot = args.bundle_id is None
        runner = lambda: run_macos_local_smoke(
            config,
            args.launch_command,
            action_name="inspect",
            bundle_id=args.bundle_id,
            label=args.label,
            output_path=args.output,
            capture_ui_snapshot=capture_ui_snapshot,
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            pulp_app_automation=False,
            capture_before=False,
            settle_secs=0.0,
            timeout_secs=args.timeout,
            source_request=source_request,
        )
    elif adapter == "linux-xvfb":
        if args.bundle_id:
            print("Error: linux-xvfb desktop inspect currently supports --command only.")
            return 1
        if not args.launch_command:
            print("Error: desktop inspect requires --command for linux-xvfb targets.")
            return 1
        runner = lambda: run_linux_xvfb_remote_action(
            config,
            args.target,
            target,
            args.launch_command,
            action_name="inspect",
            label=args.label,
            output_path=args.output,
            pulp_app_automation=getattr(args, "pulp_app_automation", False),
            capture_ui_snapshot=bool(getattr(args, "pulp_app_automation", False)),
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=False,
            settle_secs=0.0,
            timeout_secs=args.timeout,
            source_request=source_request,
        )
    elif adapter == "windows-session-agent":
        if args.bundle_id:
            print("Error: windows desktop inspect currently supports --command only.")
            return 1
        if not args.launch_command:
            print("Error: desktop inspect requires --command for windows targets.")
            return 1
        pulp_app_automation = bool(getattr(args, "pulp_app_automation", False))
        runner = lambda: run_windows_session_agent_action(
            config,
            args.target,
            target,
            args.launch_command,
            action_name="inspect",
            label=args.label,
            output_path=args.output,
            pulp_app_automation=pulp_app_automation,
            capture_ui_snapshot=pulp_app_automation,
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=False,
            settle_secs=0.0,
            timeout_secs=args.timeout,
            source_request=source_request,
        )
    else:
        print(f"Error: desktop inspect is not implemented for `{args.target}` yet; adapter `{adapter}` is still pending.")
        return 1

    try:
        manifest = runner()
    except Exception as exc:
        print(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print(json.dumps(manifest, indent=2))
        return 0

    for line in _desktop_cli.desktop_action_success_lines("inspect", args.target, manifest):
        print(line)
    return 0


def cmd_desktop(args: argparse.Namespace) -> int:
    return _cli_dispatch.dispatch_desktop_command(
        args,
        commands={
            "install": cmd_desktop_install,
            "doctor": cmd_desktop_doctor,
            "status": cmd_desktop_status,
            "config": cmd_desktop_config,
            "recent": cmd_desktop_recent,
            "proof": cmd_desktop_proof,
            "publish": cmd_desktop_publish,
            "cleanup": cmd_desktop_cleanup,
            "smoke": cmd_desktop_smoke,
            "click": cmd_desktop_click,
            "inspect": cmd_desktop_inspect,
        },
    )


def build_parser() -> argparse.ArgumentParser:
    return build_local_ci_parser(
        priority_values=PRIORITY_VALUES,
        keep_completed_jobs=KEEP_COMPLETED_JOBS,
        epilog=__doc__,
    )


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    return _cli_dispatch.dispatch_main_command(
        args,
        commands={
            "enqueue": cmd_enqueue,
            "drain": cmd_drain,
            "run": cmd_run,
            "ship": cmd_ship,
            "check": cmd_check,
            "list": cmd_list,
            "bump": cmd_bump,
            "cancel": cmd_cancel,
            "logs": cmd_logs,
            "cleanup": cmd_cleanup,
            "evidence": cmd_evidence,
            "status": cmd_status,
            "desktop": cmd_desktop,
        },
        cloud_commands={
            "workflows": cmd_cloud_workflows,
            "defaults": cmd_cloud_defaults,
            "history": cmd_cloud_history,
            "compare": cmd_cloud_compare,
            "recommend": cmd_cloud_recommend,
            "run": cmd_cloud_run,
            "status": cmd_cloud_status,
        },
        cloud_namespace_commands={
            "doctor": cmd_cloud_namespace_doctor,
            "setup": cmd_cloud_namespace_setup,
        },
        print_help=parser.print_help,
    )


if __name__ == "__main__":
    sys.exit(main())
