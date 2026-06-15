"""Facade bootstrap wiring for local_ci.py."""
from __future__ import annotations

import cleanup_bindings as _cleanup_bindings
import cli_dispatch_bindings as _cli_dispatch_bindings
import cli_parser_bindings as _cli_parser_bindings
import cloud_bindings as _cloud_bindings
import config_evidence_bindings as _config_evidence_bindings
import desktop_command_bindings as _desktop_command_bindings
import desktop_infra_bindings as _desktop_infra_bindings
import desktop_probe_bindings as _desktop_probe_bindings
import desktop_reporting_bindings as _desktop_reporting_bindings
import desktop_support_bindings as _desktop_support_bindings
import evidence_index_bindings as _evidence_index_bindings
import execution_bindings as _execution_bindings
import execution_logging_timing_bindings as _execution_logging_timing_bindings
import footprint_bindings as _footprint_bindings
import git_helpers_bindings as _git_helpers_bindings
import github_workflow_bindings as _github_workflow_bindings
import io_utils_bindings as _io_utils_bindings
import job_queue_bindings as _job_queue_bindings
import linux_desktop_bindings as _linux_desktop_bindings
import linux_target_bindings as _linux_target_bindings
import local_ci_bootstrap_constants as _local_ci_bootstrap_constants
import local_ci_bootstrap_helper_installers as _local_ci_bootstrap_helper_installers
import local_ci_bootstrap_module_aliases as _local_ci_bootstrap_module_aliases
import local_ci_bootstrap_private_seams as _local_ci_bootstrap_private_seams
import local_ci_command_bindings as _local_ci_command_bindings
import macos_desktop_bindings as _macos_desktop_bindings
import macos_window_bindings as _macos_window_bindings
import notification_bindings as _notification_bindings
import normalize_bindings as _normalize_bindings
import provenance_bindings as _provenance_bindings
import queue_bindings as _queue_bindings
import source_prep_bindings as _source_prep_bindings
import ssh_bundle_bindings as _ssh_bundle_bindings
import ssh_subprocess_bindings as _ssh_subprocess_bindings
import state_path_bindings as _state_path_bindings
import target_bindings as _target_bindings
import target_preflight_bindings as _target_preflight_bindings
import utility_command_bindings as _utility_command_bindings
import windows_desktop_bindings as _windows_desktop_bindings
import windows_probe_bindings as _windows_probe_bindings
import windows_target_bindings as _windows_target_bindings


def install_local_ci_facade(bindings: dict) -> None:
    """Install compatibility facade exports and private late-binding seams."""
    _local_ci_bootstrap_module_aliases.install_bootstrap_module_aliases(bindings)

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

    _local_ci_bootstrap_helper_installers.install_command_helpers(
        bindings,
        target_preflight_bindings=_target_preflight_bindings,
        notification_bindings=_notification_bindings,
        execution_bindings=_execution_bindings,
        private_seams=_local_ci_bootstrap_private_seams,
        utility_command_bindings=_utility_command_bindings,
        local_ci_command_bindings=_local_ci_command_bindings,
        desktop_command_bindings=_desktop_command_bindings,
        cli_dispatch_bindings=_cli_dispatch_bindings,
    )
