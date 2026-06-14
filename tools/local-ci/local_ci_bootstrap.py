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
import execution_logging_timing_bindings as _execution_logging_timing_bindings
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
import local_ci_bootstrap_constants as _local_ci_bootstrap_constants
import local_ci_bootstrap_helper_installers as _local_ci_bootstrap_helper_installers
import local_ci_bootstrap_module_aliases as _local_ci_bootstrap_module_aliases
import local_ci_bootstrap_private_seams as _local_ci_bootstrap_private_seams
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
    _local_ci_bootstrap_module_aliases.install_bootstrap_module_aliases(bindings, globals())

    _local_ci_bootstrap_helper_installers.install_foundation_helpers(
        bindings,
        state_path_bindings=_state_path_bindings,
        footprint_bindings=_footprint_bindings,
        ssh_subprocess_bindings=_ssh_subprocess_bindings,
        ssh_bundle_bindings=_ssh_bundle_bindings,
    )

    _local_ci_bootstrap_constants.install_bootstrap_constants(
        bindings,
        execution_timing_bindings=_execution_logging_timing_bindings,
        windows_target_bindings=_windows_target_bindings,
        linux_target_bindings=_linux_target_bindings,
        normalize_bindings=_normalize_bindings,
        github_workflow_bindings=_github_workflow_bindings,
    )

    _local_ci_bootstrap_helper_installers.install_core_helpers(
        bindings,
        windows_target_bindings=_windows_target_bindings,
        io_utils_bindings=_io_utils_bindings,
        git_helpers_bindings=_git_helpers_bindings,
        normalize_bindings=_normalize_bindings,
        cli_parser_bindings=_cli_parser_bindings,
        config_evidence_bindings=_config_evidence_bindings,
        github_workflow_bindings=_github_workflow_bindings,
        provenance_bindings=_provenance_bindings,
        evidence_index_bindings=_evidence_index_bindings,
        job_queue_bindings=_job_queue_bindings,
        queue_bindings=_queue_bindings,
        cleanup_bindings=_cleanup_bindings,
        target_bindings=_target_bindings,
        cloud_bindings=_cloud_bindings,
    )
    _local_ci_bootstrap_helper_installers.install_desktop_helpers(
        bindings,
        desktop_support_bindings=_desktop_support_bindings,
        desktop_infra_bindings=_desktop_infra_bindings,
        desktop_reporting_bindings=_desktop_reporting_bindings,
        macos_window_bindings=_macos_window_bindings,
        linux_target_bindings=_linux_target_bindings,
        linux_desktop_bindings=_linux_desktop_bindings,
        source_prep_bindings=_source_prep_bindings,
        windows_probe_bindings=_windows_probe_bindings,
        desktop_probe_bindings=_desktop_probe_bindings,
        macos_desktop_bindings=_macos_desktop_bindings,
        windows_desktop_bindings=_windows_desktop_bindings,
    )

    _local_ci_bootstrap_private_seams.install_desktop_private_seams(bindings)

    _target_preflight_bindings.install_target_preflight_helpers(bindings)
    _notification_bindings.install_notification_helpers(bindings)
    _execution_bindings.install_execution_helpers(bindings)
    _local_ci_bootstrap_private_seams.install_execution_private_seams(bindings)
    _utility_command_bindings.install_utility_command_helpers(bindings)
    _local_ci_command_bindings.install_local_ci_command_helpers(bindings)
    _desktop_command_bindings.install_desktop_command_helpers(bindings)
    _cli_dispatch_bindings.install_cli_dispatch_helpers(bindings)
