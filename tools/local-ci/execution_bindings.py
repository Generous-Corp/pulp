"""Bindings from the local_ci facade to validation execution helpers."""

from __future__ import annotations

from collections.abc import Mapping
import builtins
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def _print_binding(bindings: Mapping[str, Any]) -> Any:
    return bindings.get("print", builtins.print)


def run_local_validation(
    bindings: Mapping[str, Any],
    job: dict,
    exclude_tests: str = "",
    report_progress=None,
) -> dict:
    return _binding(bindings, "_execution").run_local_validation(
        job,
        exclude_tests,
        report_progress,
        root=_binding(bindings, "ROOT"),
        print_fn=_print_binding(bindings),
        short_sha_fn=_binding(bindings, "short_sha"),
        prepare_target_log_fn=_binding(bindings, "prepare_target_log"),
        now_iso_fn=_binding(bindings, "now_iso"),
        local_validation_command_fn=_binding(bindings, "local_validation_command"),
        run_logged_command_fn=_binding(bindings, "run_logged_command"),
        validation_result_from_run_fn=_binding(bindings, "validation_result_from_run"),
    )


def run_posix_ssh_validation(
    bindings: Mapping[str, Any],
    target_name: str,
    host: str,
    repo_path: str,
    job: dict,
    exclude_tests: str = "",
    config: dict | None = None,
    report_progress=None,
) -> dict:
    return _binding(bindings, "_execution").run_posix_ssh_validation(
        target_name,
        host,
        repo_path,
        job,
        exclude_tests,
        config,
        report_progress,
        print_fn=_print_binding(bindings),
        short_sha_fn=_binding(bindings, "short_sha"),
        prepare_target_log_fn=_binding(bindings, "prepare_target_log"),
        now_iso_fn=_binding(bindings, "now_iso"),
        sync_job_bundle_to_ssh_host_fn=_binding(bindings, "sync_job_bundle_to_ssh_host"),
        posix_ssh_validation_command_fn=_binding(bindings, "posix_ssh_validation_command"),
        run_logged_command_fn=_binding(bindings, "run_logged_command"),
        validation_result_from_run_fn=_binding(bindings, "validation_result_from_run"),
        validation_error_result_fn=_binding(bindings, "validation_error_result"),
    )


def run_windows_ssh_validation(
    bindings: Mapping[str, Any],
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
    return _binding(bindings, "_execution").run_windows_ssh_validation(
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
        root=_binding(bindings, "ROOT"),
        prepare_target_log_fn=_binding(bindings, "prepare_target_log"),
        sync_job_bundle_to_ssh_host_fn=_binding(bindings, "sync_job_bundle_to_ssh_host"),
        validation_error_result_fn=_binding(bindings, "validation_error_result"),
        ensure_windows_remote_repo_checkout_fn=_binding(bindings, "ensure_windows_remote_repo_checkout"),
        git_origin_clone_url_fn=_binding(bindings, "git_origin_clone_url"),
        print_fn=_print_binding(bindings),
        short_sha_fn=_binding(bindings, "short_sha"),
        now_iso_fn=_binding(bindings, "now_iso"),
        probe_windows_ssh_cmake_settings_fn=_binding(bindings, "probe_windows_ssh_cmake_settings"),
        windows_validation_script_fn=_binding(bindings, "windows_validation_script"),
        windows_ssh_powershell_command_fn=_binding(bindings, "windows_ssh_powershell_command"),
        run_logged_command_fn=_binding(bindings, "run_logged_command"),
        validation_result_from_run_fn=_binding(bindings, "validation_result_from_run"),
    )
