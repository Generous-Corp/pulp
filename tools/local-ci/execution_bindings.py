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


def config_for_job_execution(bindings: Mapping[str, Any], job: dict, config: dict) -> dict:
    return _binding(bindings, "_execution").config_for_job_execution(
        job,
        config,
        load_config_file_fn=_binding(bindings, "load_config_file"),
        warn_fn=_print_binding(bindings),
    )


def resolve_ssh_target_execution(
    bindings: Mapping[str, Any],
    job: dict,
    target_name: str,
    target_cfg: dict,
    defaults: dict,
) -> tuple[str | None, str | None]:
    return _binding(bindings, "_execution").resolve_ssh_target_execution(
        job,
        target_name,
        target_cfg,
        defaults,
        ensure_host_reachable_fn=_binding(bindings, "ensure_host_reachable"),
    )


def build_target_tasks(bindings: Mapping[str, Any], job: dict, config: dict, progress_factory=None) -> list[tuple[str, Any]]:
    return _binding(bindings, "_execution").build_target_tasks(
        job,
        config,
        enabled_targets_fn=_binding(bindings, "enabled_targets"),
        resolve_ssh_target_execution_fn=_binding(bindings, "resolve_ssh_target_execution"),
        run_local_validation_fn=_binding(bindings, "run_local_validation"),
        run_posix_ssh_validation_fn=_binding(bindings, "run_posix_ssh_validation"),
        run_windows_ssh_validation_fn=_binding(bindings, "run_windows_ssh_validation"),
        progress_factory=progress_factory,
    )


def process_job(bindings: Mapping[str, Any], job: dict, config: dict) -> dict:
    return _binding(bindings, "_execution").process_job(
        job,
        config,
        print_fn=_print_binding(bindings),
        short_sha_fn=_binding(bindings, "short_sha"),
        config_for_job_execution_fn=_binding(bindings, "config_for_job_execution"),
        build_target_tasks_fn=_binding(bindings, "_build_target_tasks"),
        target_state_snapshot_fn=_binding(bindings, "target_state_snapshot"),
        update_runner_active_targets_fn=_binding(bindings, "update_runner_active_targets"),
        update_job_active_targets_fn=_binding(bindings, "update_job_active_targets"),
        updated_target_state_fn=_binding(bindings, "updated_target_state"),
        initial_target_state_fn=_binding(bindings, "initial_target_state"),
        completed_target_state_fn=_binding(bindings, "completed_target_state"),
        now_iso_fn=_binding(bindings, "now_iso"),
        run_target_tasks_fn=_binding(bindings, "run_target_tasks"),
        completed_job_result_fn=_binding(bindings, "completed_job_result"),
        sorted_target_results_fn=_binding(bindings, "sorted_target_results"),
    )


def save_result(bindings: Mapping[str, Any], result: dict) -> Any:
    return _binding(bindings, "_execution").save_result(
        result,
        ensure_state_dirs_fn=_binding(bindings, "ensure_state_dirs"),
        results_dir_fn=_binding(bindings, "results_dir"),
        update_evidence_index_fn=_binding(bindings, "update_evidence_index"),
        now_fn=_binding(bindings, "datetime").now,
    )


def print_result(bindings: Mapping[str, Any], result: dict, result_path=None) -> None:
    return _binding(bindings, "_execution").print_result(
        result,
        result_path,
        normalize_result_fn=_binding(bindings, "normalize_result"),
        result_validation_line_fn=_binding(bindings, "result_validation_line"),
        result_execution_line_fn=_binding(bindings, "result_execution_line"),
        result_target_lines_fn=_binding(bindings, "result_target_lines"),
        result_overall_line_fn=_binding(bindings, "result_overall_line"),
        print_fn=_print_binding(bindings),
    )
