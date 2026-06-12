"""Facade bindings for validation runner helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import print_binding as _print_binding


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


def windows_validation_script(
    bindings: Mapping[str, Any],
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
    return _binding(bindings, "_execution").windows_validation_script(
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
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )
