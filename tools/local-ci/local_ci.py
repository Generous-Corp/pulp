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

from collections import deque
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


_github_workflow_bindings.install_github_workflow_helpers(globals())


_provenance_bindings.install_provenance_helpers(globals())


import evidence_index as evidence_index_module  # noqa: E402

_evidence_index_bindings.install_evidence_index_helpers(globals())


_job_queue_bindings.install_job_queue_helpers(globals())
_queue_bindings.install_queue_helpers(globals())
_cleanup_bindings.install_cleanup_helpers(globals())


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


_target_preflight_bindings.install_target_preflight_helpers(globals())
_notification_bindings.install_notification_helpers(globals())
_execution_bindings.install_execution_helpers(globals())
_build_target_tasks = build_target_tasks


# ── VM Management ────────────────────────────────────────────────────────────


# ── Validation Runners ───────────────────────────────────────────────────────


# ── Job Processing ───────────────────────────────────────────────────────────


# ── GitHub Helpers ───────────────────────────────────────────────────────────


_utility_command_bindings.install_utility_command_helpers(globals())


_local_ci_command_bindings.install_local_ci_command_helpers(globals())

_desktop_command_bindings.install_desktop_command_helpers(globals())
_cli_dispatch_bindings.install_cli_dispatch_helpers(globals())


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    return _cli_dispatch_bindings.dispatch_main_command(globals(), args, parser.print_help)


if __name__ == "__main__":
    sys.exit(main())
