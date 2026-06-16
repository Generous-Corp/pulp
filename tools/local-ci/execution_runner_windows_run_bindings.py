"""Bindings from the local_ci facade to the Windows SSH validation runner."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from binding_utils import print_binding as _print_binding


EXECUTION_RUNNER_WINDOWS_RUN_EXPORTS = ("run_windows_ssh_validation",)


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


def install_execution_runner_windows_run_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_RUNNER_WINDOWS_RUN_EXPORTS,
) -> None:
    known_names = set(EXECUTION_RUNNER_WINDOWS_RUN_EXPORTS)
    runner_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), runner_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
