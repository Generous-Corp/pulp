"""Facade bindings for POSIX SSH validation runner helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from binding_utils import print_binding as _print_binding


EXECUTION_RUNNER_SSH_EXPORTS = ("run_posix_ssh_validation",)


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


def install_execution_runner_ssh_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_RUNNER_SSH_EXPORTS,
) -> None:
    known_names = set(EXECUTION_RUNNER_SSH_EXPORTS)
    runner_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), runner_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
