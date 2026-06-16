"""Ordered helper installer groups for the local_ci.py facade bootstrap."""

from __future__ import annotations

from typing import Any


def install_foundation_helpers(
    bindings: dict[str, Any],
    *,
    state_path_bindings: Any,
    footprint_bindings: Any,
    ssh_subprocess_bindings: Any,
    ssh_bundle_bindings: Any,
) -> None:
    state_path_bindings.install_state_path_helpers(bindings)
    footprint_bindings.install_footprint_helpers(bindings)
    ssh_subprocess_bindings.install_ssh_subprocess_helpers(bindings)
    ssh_bundle_bindings.install_ssh_bundle_helpers(bindings)


def install_core_helpers(
    bindings: dict[str, Any],
    *,
    windows_target_bindings: Any,
    io_utils_bindings: Any,
    git_helpers_bindings: Any,
    normalize_bindings: Any,
    cli_parser_bindings: Any,
    config_evidence_bindings: Any,
    github_workflow_bindings: Any,
    provenance_bindings: Any,
    evidence_index_bindings: Any,
    job_queue_bindings: Any,
    queue_bindings: Any,
    cleanup_bindings: Any,
    target_bindings: Any,
    cloud_bindings: Any,
) -> None:
    windows_target_bindings.install_windows_target_helpers(bindings)
    io_utils_bindings.install_io_utils_helpers(bindings)
    git_helpers_bindings.install_git_helpers(bindings)
    normalize_bindings.install_normalize_helpers(bindings)
    cli_parser_bindings.install_cli_parser_helpers(bindings)
    config_evidence_bindings.install_config_evidence_helpers(bindings)
    github_workflow_bindings.install_github_workflow_helpers(bindings)
    provenance_bindings.install_provenance_helpers(bindings)
    evidence_index_bindings.install_evidence_index_helpers(bindings)
    job_queue_bindings.install_job_queue_helpers(bindings)
    queue_bindings.install_queue_helpers(bindings)
    cleanup_bindings.install_cleanup_helpers(bindings)
    target_bindings.install_target_helpers(bindings)
    cloud_bindings.install_cloud_helpers(bindings)


def install_desktop_helpers(
    bindings: dict[str, Any],
    *,
    desktop_support_bindings: Any,
    desktop_infra_bindings: Any,
    desktop_reporting_bindings: Any,
    macos_window_bindings: Any,
    linux_target_bindings: Any,
    linux_desktop_bindings: Any,
    source_prep_bindings: Any,
    windows_probe_bindings: Any,
    desktop_probe_bindings: Any,
    macos_desktop_bindings: Any,
    macos_video_bindings: Any,
    windows_desktop_bindings: Any,
) -> None:
    desktop_support_bindings.install_desktop_support_helpers(bindings)
    desktop_infra_bindings.install_desktop_infra_helpers(bindings)
    desktop_reporting_bindings.install_desktop_reporting_helpers(bindings)
    macos_window_bindings.install_macos_window_helpers(bindings)
    linux_target_bindings.install_linux_target_helpers(bindings)
    linux_desktop_bindings.install_linux_desktop_helpers(bindings)
    source_prep_bindings.install_source_prep_helpers(bindings)
    windows_probe_bindings.install_windows_probe_helpers(bindings)
    desktop_probe_bindings.install_desktop_probe_helpers(bindings)
    macos_desktop_bindings.install_macos_desktop_helpers(bindings)
    macos_video_bindings.install_macos_video_helpers(bindings)
    windows_desktop_bindings.install_windows_desktop_helpers(bindings)


def install_command_helpers(
    bindings: dict[str, Any],
    *,
    target_preflight_bindings: Any,
    notification_bindings: Any,
    execution_bindings: Any,
    private_seams: Any,
    utility_command_bindings: Any,
    local_ci_command_bindings: Any,
    desktop_command_bindings: Any,
    cli_dispatch_bindings: Any,
) -> None:
    target_preflight_bindings.install_target_preflight_helpers(bindings)
    notification_bindings.install_notification_helpers(bindings)
    execution_bindings.install_execution_helpers(bindings)
    private_seams.install_execution_private_seams(bindings)
    utility_command_bindings.install_utility_command_helpers(bindings)
    local_ci_command_bindings.install_local_ci_command_helpers(bindings)
    desktop_command_bindings.install_desktop_command_helpers(bindings)
    cli_dispatch_bindings.install_cli_dispatch_helpers(bindings)
