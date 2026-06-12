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


import state_paths as _state_paths  # noqa: E402
import state_path_bindings as _state_path_bindings  # noqa: E402

_state_path_bindings.install_state_path_helpers(globals())


import footprint as _footprint  # noqa: E402
import footprint_bindings as _footprint_bindings  # noqa: E402

_footprint_bindings.install_footprint_helpers(globals())

import cleanup as _cleanup  # noqa: E402
import cleanup_bindings as _cleanup_bindings  # noqa: E402
import cleanup_cli as _cleanup_cli  # noqa: E402
import cli_dispatch as _cli_dispatch  # noqa: E402
import cli_dispatch_bindings as _cli_dispatch_bindings  # noqa: E402
import cli_parser_bindings as _cli_parser_bindings  # noqa: E402
import cloud as _cloud  # noqa: E402
import cloud_bindings as _cloud_bindings  # noqa: E402
import config_evidence_bindings as _config_evidence_bindings  # noqa: E402
import desktop_action_commands_cli as _desktop_action_commands_cli  # noqa: E402
import desktop_actions as _desktop_actions  # noqa: E402
import desktop_artifacts as _desktop_artifacts  # noqa: E402
import desktop_command_bindings as _desktop_command_bindings  # noqa: E402
import desktop_commands_cli as _desktop_commands_cli  # noqa: E402
import desktop_cli as _desktop_cli  # noqa: E402
import desktop_doctor as _desktop_doctor  # noqa: E402
import desktop_infra_bindings as _desktop_infra_bindings  # noqa: E402
import desktop_probe_bindings as _desktop_probe_bindings  # noqa: E402
import desktop_reporting_bindings as _desktop_reporting_bindings  # noqa: E402
import desktop_setup_commands_cli as _desktop_setup_commands_cli  # noqa: E402
import desktop_support_bindings as _desktop_support_bindings  # noqa: E402
import evidence_cli as _evidence_cli  # noqa: E402
import evidence_index_bindings as _evidence_index_bindings  # noqa: E402
import git_helpers as _git_helpers  # noqa: E402
import git_helpers_bindings as _git_helpers_bindings  # noqa: E402
import github_workflows as _github_workflows  # noqa: E402
import github_workflow_bindings as _github_workflow_bindings  # noqa: E402
import io_utils as _io_utils  # noqa: E402
import io_utils_bindings as _io_utils_bindings  # noqa: E402
import job_queue as _job_queue  # noqa: E402
import job_queue_bindings as _job_queue_bindings  # noqa: E402
import linux_desktop_action as _linux_desktop_action  # noqa: E402
import linux_desktop_bindings as _linux_desktop_bindings  # noqa: E402
import linux_target as _linux_target  # noqa: E402
import linux_target_bindings as _linux_target_bindings  # noqa: E402
import local_ci_command_bindings as _local_ci_command_bindings  # noqa: E402
import local_ci_commands_cli as _local_ci_commands_cli  # noqa: E402
import logs_cli as _logs_cli  # noqa: E402
import macos_desktop as _macos_desktop  # noqa: E402
import macos_desktop_action as _macos_desktop_action  # noqa: E402
import macos_desktop_bindings as _macos_desktop_bindings  # noqa: E402
import macos_window_bindings as _macos_window_bindings  # noqa: E402
import notifications as _notifications  # noqa: E402
import notification_bindings as _notification_bindings  # noqa: E402
import normalize as _normalize  # noqa: E402
import normalize_bindings as _normalize_bindings  # noqa: E402
import provenance as _provenance  # noqa: E402
import provenance_bindings as _provenance_bindings  # noqa: E402
import queue_commands_cli as _queue_commands_cli  # noqa: E402
import queue_bindings as _queue_bindings  # noqa: E402
import queue_lifecycle as _queue_lifecycle  # noqa: E402
import queue_orchestrator as _queue_orchestrator  # noqa: E402
import execution as _execution  # noqa: E402
import execution_bindings as _execution_bindings  # noqa: E402

HEARTBEAT_INTERVAL_SECS = _execution_bindings.heartbeat_interval_secs(globals())
STUCK_IDLE_SECS = _execution_bindings.stuck_idle_secs(globals())
import reporting as _reporting  # noqa: E402
import runner_state as _runner_state  # noqa: E402
import source_prep as _source_prep  # noqa: E402
import source_prep_bindings as _source_prep_bindings  # noqa: E402
import ssh_bundle as _ssh_bundle  # noqa: E402
import ssh_bundle_bindings as _ssh_bundle_bindings  # noqa: E402
import ssh_subprocess as _ssh_subprocess  # noqa: E402
import ssh_subprocess_bindings as _ssh_subprocess_bindings  # noqa: E402
import target_preflight as _target_preflight  # noqa: E402
import target_preflight_bindings as _target_preflight_bindings  # noqa: E402
import targets as _targets  # noqa: E402
import target_bindings as _target_bindings  # noqa: E402
import utility_command_bindings as _utility_command_bindings  # noqa: E402
import windows_desktop_action as _windows_desktop_action  # noqa: E402
import windows_desktop_bindings as _windows_desktop_bindings  # noqa: E402
import windows_probe as _windows_probe  # noqa: E402
import windows_probe_bindings as _windows_probe_bindings  # noqa: E402
import windows_target as _windows_target  # noqa: E402
import windows_target_bindings as _windows_target_bindings  # noqa: E402

_ssh_subprocess_bindings.install_ssh_subprocess_helpers(globals())
_ssh_bundle_bindings.install_ssh_bundle_helpers(globals())

WINDOWS_REQUIRED_REMOTE_TOOLS = _windows_target_bindings.windows_required_remote_tools(globals())
WINDOWS_OPTIONAL_REMOTE_TOOLS = _windows_target_bindings.windows_optional_remote_tools(globals())
WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME = _windows_target_bindings.windows_default_remote_repo_dirname(globals())
LINUX_REQUIRED_REMOTE_TOOLS = _linux_target_bindings.linux_required_remote_tools(globals())
LINUX_OPTIONAL_REMOTE_TOOLS = _linux_target_bindings.linux_optional_remote_tools(globals())
_windows_target_bindings.install_windows_target_helpers(globals())


from io_utils import LockBusyError  # noqa: E402  -- re-exported for callers catching the facade class

_io_utils_bindings.install_io_utils_helpers(globals())


_git_helpers_bindings.install_git_helpers(globals())


PRIORITY_VALUES = _normalize_bindings.priority_values(globals())


_normalize_bindings.install_normalize_helpers(globals())

import cli_parser as _cli_parser  # noqa: E402


_cli_parser_bindings.install_cli_parser_helpers(globals())


_config_evidence_bindings.install_config_evidence_helpers(globals())


GITHUB_ACTIONS_DEFAULTS = _github_workflow_bindings.github_actions_defaults(globals())
BUILTIN_GITHUB_WORKFLOWS = _github_workflow_bindings.builtin_github_workflows(globals())
REPO_VARIABLE_FALLBACKS = _github_workflow_bindings.repo_variable_fallbacks(globals())


def github_actions_settings_for_display(config: dict | None) -> dict:
    return _github_workflow_bindings.github_actions_settings_for_display(globals(), config)


def resolve_github_actions_settings(config: dict | None) -> dict:
    return _github_workflow_bindings.resolve_github_actions_settings(globals(), config)


def normalize_runs_on_json(raw: str, *, setting_name: str) -> str:
    return _github_workflow_bindings.normalize_runs_on_json(globals(), raw, setting_name=setting_name)


def resolve_workflow_runner_selector_json(config: dict | None, workflow_key: str, provider: str) -> str:
    return _github_workflow_bindings.resolve_workflow_runner_selector_json(globals(), config, workflow_key, provider)


def resolve_workflow_dispatch_field_values(
    config: dict | None,
    workflow_key: str,
    provider: str,
    field_names: list[str] | tuple[str, ...] | None,
) -> dict[str, str]:
    return _github_workflow_bindings.resolve_workflow_dispatch_field_values(
        globals(),
        config,
        workflow_key,
        provider,
        field_names,
    )


def repo_variable_name_for_workflow_field(workflow_key: str, provider: str, field_name: str) -> str:
    return _github_workflow_bindings.repo_variable_name_for_workflow_field(globals(), workflow_key, provider, field_name)


def resolve_default_provider_for_workflow(
    settings: dict,
    workflow_key: str,
    *,
    explicit_provider: str | None = None,
) -> tuple[str, str]:
    return _github_workflow_bindings.resolve_default_provider_for_workflow(
        globals(),
        settings,
        workflow_key,
        explicit_provider=explicit_provider,
    )


def resolve_workflow_field_value_and_source(
    config: dict | None,
    repository_variables: dict[str, str],
    workflow_key: str,
    provider: str,
    field_name: str,
) -> tuple[str, str]:
    return _github_workflow_bindings.resolve_workflow_field_value_and_source(
        globals(),
        config,
        repository_variables,
        workflow_key,
        provider,
        field_name,
    )


def resolve_workflow_dispatch_defaults(
    config: dict | None,
    repository_variables: dict[str, str],
    workflow_key: str,
    provider: str,
    field_names: list[str] | tuple[str, ...] | None,
) -> tuple[dict[str, str], dict[str, str]]:
    return _github_workflow_bindings.resolve_workflow_dispatch_defaults(
        globals(),
        config,
        repository_variables,
        workflow_key,
        provider,
        field_names,
    )


def summarize_workflow_provider_defaults(
    config: dict | None,
    repository_variables: dict[str, str],
    settings: dict,
    workflow_key: str,
) -> dict:
    return _github_workflow_bindings.summarize_workflow_provider_defaults(
        globals(),
        config,
        repository_variables,
        settings,
        workflow_key,
    )


def resolve_cli_dispatch_field_values(args: argparse.Namespace, field_names: list[str] | tuple[str, ...] | None) -> dict[str, str]:
    return _github_workflow_bindings.resolve_cli_dispatch_field_values(globals(), args, field_names)


_provenance_bindings.install_provenance_helpers(globals())


import evidence_index as evidence_index_module  # noqa: E402

_evidence_index_bindings.install_evidence_index_helpers(globals())


_job_queue_bindings.install_job_queue_helpers(globals())


def load_queue() -> list[dict]:
    return _queue_bindings.load_queue(globals())


_target_bindings.install_target_helpers(globals())


_cloud_bindings.install_cloud_helpers(globals())
_desktop_support_bindings.install_desktop_support_helpers(globals())
_desktop_infra_bindings.install_desktop_infra_helpers(globals())
_desktop_reporting_bindings.install_desktop_reporting_helpers(globals())
_macos_window_bindings.install_macos_window_helpers(globals())
_linux_target_bindings.install_linux_target_helpers(globals())
_linux_desktop_bindings.install_linux_desktop_helpers(globals())
_source_prep_bindings.install_source_prep_helpers(globals())
_windows_probe_bindings.install_windows_probe_helpers(globals())
_desktop_probe_bindings.install_desktop_probe_helpers(globals())
_macos_desktop_bindings.install_macos_desktop_helpers(globals())
_windows_desktop_bindings.install_windows_desktop_helpers(globals())
_desktop_check = desktop_check
_check_writable_dir = check_writable_dir
_clear_directory_contents = clear_directory_contents
_copy_directory_contents = copy_directory_contents
_run_git = run_git
_command_path_rewrite_candidate = command_path_rewrite_candidate
_rewrite_launch_command_for_mapper = rewrite_launch_command_for_mapper
_local_worktree_matches = local_worktree_matches
_reset_local_worktree = reset_local_worktree


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
    return _cleanup_bindings.result_file_job_id(globals(), path)


def artifact_entry_sort_key(entry: dict) -> tuple[float, str]:
    return _cleanup_bindings.artifact_entry_sort_key(globals(), entry)


def collect_local_ci_cleanup_plan(
    queue: list[dict],
    *,
    keep_results: int = KEEP_COMPLETED_JOBS,
    keep_logs: int = KEEP_COMPLETED_JOBS,
    keep_bundles: int = 0,
    include_prepared: bool = False,
) -> dict:
    return _cleanup_bindings.collect_local_ci_cleanup_plan(
        globals(),
        queue,
        keep_results=keep_results,
        keep_logs=keep_logs,
        keep_bundles=keep_bundles,
        include_prepared=include_prepared,
    )


def apply_local_ci_cleanup_plan(plan: dict) -> dict:
    return _cleanup_bindings.apply_local_ci_cleanup_plan(globals(), plan)


def cleanup_plan_lines(plan: dict, *, dry_run: bool) -> list[str]:
    return _cleanup_bindings.cleanup_plan_lines(globals(), plan, dry_run=dry_run)


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
    return _cleanup_bindings.collect_stale_windows_cleanup_candidates_unlocked(globals(), queue)


def cleanup_stale_windows_validator(host: str, pid: int, started_at: str) -> dict:
    return _cleanup_bindings.cleanup_stale_windows_validator(globals(), host, pid, started_at)


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


_target_preflight_bindings.install_target_preflight_helpers(globals())
_notification_bindings.install_notification_helpers(globals())


# ── VM Management ────────────────────────────────────────────────────────────


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


_utility_command_bindings.install_utility_command_helpers(globals())


_local_ci_command_bindings.install_local_ci_command_helpers(globals())

_desktop_command_bindings.install_desktop_command_helpers(globals())


def cmd_desktop_config(args: argparse.Namespace) -> int:
    return _cli_dispatch_bindings.cmd_desktop_config(globals(), args)


def cmd_desktop(args: argparse.Namespace) -> int:
    return _cli_dispatch_bindings.cmd_desktop(globals(), args)


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    return _cli_dispatch_bindings.dispatch_main_command(globals(), args, parser.print_help)


if __name__ == "__main__":
    sys.exit(main())
