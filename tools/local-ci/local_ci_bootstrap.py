"""Facade bootstrap wiring for local_ci.py."""
from __future__ import annotations

import cleanup as _cleanup
import cleanup_bindings as _cleanup_bindings
import cleanup_cli as _cleanup_cli
import cli_dispatch as _cli_dispatch
import cli_dispatch_bindings as _cli_dispatch_bindings
import cli_parser as _cli_parser
import cli_parser_bindings as _cli_parser_bindings
import cloud as _cloud
import cloud_bindings as _cloud_bindings
import config_evidence_bindings as _config_evidence_bindings
import desktop_action_commands_cli as _desktop_action_commands_cli
import desktop_actions as _desktop_actions
import desktop_artifacts as _desktop_artifacts
import desktop_command_bindings as _desktop_command_bindings
import desktop_commands_cli as _desktop_commands_cli
import desktop_cli as _desktop_cli
import desktop_doctor as _desktop_doctor
import desktop_infra_bindings as _desktop_infra_bindings
import desktop_probe_bindings as _desktop_probe_bindings
import desktop_reporting_bindings as _desktop_reporting_bindings
import desktop_setup_commands_cli as _desktop_setup_commands_cli
import desktop_support_bindings as _desktop_support_bindings
import evidence_cli as _evidence_cli
import evidence_index as _evidence_index
import evidence_index_bindings as _evidence_index_bindings
import execution as _execution
import execution_bindings as _execution_bindings
import footprint as _footprint
import footprint_bindings as _footprint_bindings
import git_helpers as _git_helpers
import git_helpers_bindings as _git_helpers_bindings
import github_workflows as _github_workflows
import github_workflow_bindings as _github_workflow_bindings
import io_utils as _io_utils
import io_utils_bindings as _io_utils_bindings
from io_utils import LockBusyError
import job_queue as _job_queue
import job_queue_bindings as _job_queue_bindings
import linux_desktop_action as _linux_desktop_action
import linux_desktop_bindings as _linux_desktop_bindings
import linux_target as _linux_target
import linux_target_bindings as _linux_target_bindings
import local_ci_command_bindings as _local_ci_command_bindings
import local_ci_commands_cli as _local_ci_commands_cli
import logs_cli as _logs_cli
import macos_desktop as _macos_desktop
import macos_desktop_action as _macos_desktop_action
import macos_desktop_bindings as _macos_desktop_bindings
import macos_window_bindings as _macos_window_bindings
import notification_bindings as _notification_bindings
import notifications as _notifications
import normalize as _normalize
import normalize_bindings as _normalize_bindings
import provenance as _provenance
import provenance_bindings as _provenance_bindings
import queue_bindings as _queue_bindings
import queue_commands_cli as _queue_commands_cli
import queue_lifecycle as _queue_lifecycle
import queue_orchestrator as _queue_orchestrator
import reporting as _reporting
import runner_state as _runner_state
import source_prep as _source_prep
import source_prep_bindings as _source_prep_bindings
import ssh_bundle as _ssh_bundle
import ssh_bundle_bindings as _ssh_bundle_bindings
import ssh_subprocess as _ssh_subprocess
import ssh_subprocess_bindings as _ssh_subprocess_bindings
import state_path_bindings as _state_path_bindings
import state_paths as _state_paths
import target_bindings as _target_bindings
import target_preflight as _target_preflight
import target_preflight_bindings as _target_preflight_bindings
import targets as _targets
import utility_command_bindings as _utility_command_bindings
import windows_desktop_action as _windows_desktop_action
import windows_desktop_bindings as _windows_desktop_bindings
import windows_probe as _windows_probe
import windows_probe_bindings as _windows_probe_bindings
import windows_target as _windows_target
import windows_target_bindings as _windows_target_bindings


def install_local_ci_facade(bindings: dict) -> None:
    """Install compatibility facade exports and private late-binding seams."""
    bindings.update(
        {
            "_state_paths": _state_paths,
            "_footprint": _footprint,
            "_cleanup": _cleanup,
            "_cleanup_cli": _cleanup_cli,
            "_cli_dispatch": _cli_dispatch,
            "_cli_parser": _cli_parser,
            "_cloud": _cloud,
            "_desktop_action_commands_cli": _desktop_action_commands_cli,
            "_desktop_actions": _desktop_actions,
            "_desktop_artifacts": _desktop_artifacts,
            "_desktop_commands_cli": _desktop_commands_cli,
            "_desktop_cli": _desktop_cli,
            "_desktop_doctor": _desktop_doctor,
            "_desktop_setup_commands_cli": _desktop_setup_commands_cli,
            "_evidence_cli": _evidence_cli,
            "_execution": _execution,
            "_git_helpers": _git_helpers,
            "_github_workflows": _github_workflows,
            "_io_utils": _io_utils,
            "_job_queue": _job_queue,
            "_linux_desktop_action": _linux_desktop_action,
            "_linux_target": _linux_target,
            "_local_ci_commands_cli": _local_ci_commands_cli,
            "_logs_cli": _logs_cli,
            "_macos_desktop": _macos_desktop,
            "_macos_desktop_action": _macos_desktop_action,
            "_notifications": _notifications,
            "_normalize": _normalize,
            "_provenance": _provenance,
            "_queue_commands_cli": _queue_commands_cli,
            "_queue_lifecycle": _queue_lifecycle,
            "_queue_orchestrator": _queue_orchestrator,
            "_reporting": _reporting,
            "_runner_state": _runner_state,
            "_source_prep": _source_prep,
            "_ssh_bundle": _ssh_bundle,
            "_ssh_subprocess": _ssh_subprocess,
            "_target_preflight": _target_preflight,
            "_targets": _targets,
            "_windows_desktop_action": _windows_desktop_action,
            "_windows_probe": _windows_probe,
            "_windows_target": _windows_target,
            "evidence_index_module": _evidence_index,
            "LockBusyError": LockBusyError,
        }
    )

    _state_path_bindings.install_state_path_helpers(bindings)
    _footprint_bindings.install_footprint_helpers(bindings)
    _ssh_subprocess_bindings.install_ssh_subprocess_helpers(bindings)
    _ssh_bundle_bindings.install_ssh_bundle_helpers(bindings)

    bindings["HEARTBEAT_INTERVAL_SECS"] = _execution_bindings.heartbeat_interval_secs(bindings)
    bindings["STUCK_IDLE_SECS"] = _execution_bindings.stuck_idle_secs(bindings)
    bindings["WINDOWS_REQUIRED_REMOTE_TOOLS"] = (
        _windows_target_bindings.windows_required_remote_tools(bindings)
    )
    bindings["WINDOWS_OPTIONAL_REMOTE_TOOLS"] = (
        _windows_target_bindings.windows_optional_remote_tools(bindings)
    )
    bindings["WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME"] = (
        _windows_target_bindings.windows_default_remote_repo_dirname(bindings)
    )
    bindings["LINUX_REQUIRED_REMOTE_TOOLS"] = (
        _linux_target_bindings.linux_required_remote_tools(bindings)
    )
    bindings["LINUX_OPTIONAL_REMOTE_TOOLS"] = (
        _linux_target_bindings.linux_optional_remote_tools(bindings)
    )
    bindings["PRIORITY_VALUES"] = _normalize_bindings.priority_values(bindings)
    bindings["GITHUB_ACTIONS_DEFAULTS"] = (
        _github_workflow_bindings.github_actions_defaults(bindings)
    )
    bindings["BUILTIN_GITHUB_WORKFLOWS"] = (
        _github_workflow_bindings.builtin_github_workflows(bindings)
    )
    bindings["REPO_VARIABLE_FALLBACKS"] = (
        _github_workflow_bindings.repo_variable_fallbacks(bindings)
    )

    _windows_target_bindings.install_windows_target_helpers(bindings)
    _io_utils_bindings.install_io_utils_helpers(bindings)
    _git_helpers_bindings.install_git_helpers(bindings)
    _normalize_bindings.install_normalize_helpers(bindings)
    _cli_parser_bindings.install_cli_parser_helpers(bindings)
    _config_evidence_bindings.install_config_evidence_helpers(bindings)
    _github_workflow_bindings.install_github_workflow_helpers(bindings)
    _provenance_bindings.install_provenance_helpers(bindings)
    _evidence_index_bindings.install_evidence_index_helpers(bindings)
    _job_queue_bindings.install_job_queue_helpers(bindings)
    _queue_bindings.install_queue_helpers(bindings)
    _cleanup_bindings.install_cleanup_helpers(bindings)
    _target_bindings.install_target_helpers(bindings)
    _cloud_bindings.install_cloud_helpers(bindings)
    _desktop_support_bindings.install_desktop_support_helpers(bindings)
    _desktop_infra_bindings.install_desktop_infra_helpers(bindings)
    _desktop_reporting_bindings.install_desktop_reporting_helpers(bindings)
    _macos_window_bindings.install_macos_window_helpers(bindings)
    _linux_target_bindings.install_linux_target_helpers(bindings)
    _linux_desktop_bindings.install_linux_desktop_helpers(bindings)
    _source_prep_bindings.install_source_prep_helpers(bindings)
    _windows_probe_bindings.install_windows_probe_helpers(bindings)
    _desktop_probe_bindings.install_desktop_probe_helpers(bindings)
    _macos_desktop_bindings.install_macos_desktop_helpers(bindings)
    _windows_desktop_bindings.install_windows_desktop_helpers(bindings)

    bindings["_desktop_check"] = bindings["desktop_check"]
    bindings["_check_writable_dir"] = bindings["check_writable_dir"]
    bindings["_clear_directory_contents"] = bindings["clear_directory_contents"]
    bindings["_copy_directory_contents"] = bindings["copy_directory_contents"]
    bindings["_run_git"] = bindings["run_git"]
    bindings["_command_path_rewrite_candidate"] = bindings["command_path_rewrite_candidate"]
    bindings["_rewrite_launch_command_for_mapper"] = bindings["rewrite_launch_command_for_mapper"]
    bindings["_local_worktree_matches"] = bindings["local_worktree_matches"]
    bindings["_reset_local_worktree"] = bindings["reset_local_worktree"]

    _target_preflight_bindings.install_target_preflight_helpers(bindings)
    _notification_bindings.install_notification_helpers(bindings)
    _execution_bindings.install_execution_helpers(bindings)
    bindings["_build_target_tasks"] = bindings["build_target_tasks"]
    _utility_command_bindings.install_utility_command_helpers(bindings)
    _local_ci_command_bindings.install_local_ci_command_helpers(bindings)
    _desktop_command_bindings.install_desktop_command_helpers(bindings)
    _cli_dispatch_bindings.install_cli_dispatch_helpers(bindings)
