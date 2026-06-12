"""Bindings from the local_ci facade to validation execution helpers."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from binding_utils import print_binding as _print_binding
from execution_job_bindings import (
    build_target_tasks,
    config_for_job_execution,
    print_result,
    process_job,
    resolve_ssh_target_execution,
    save_result,
    submission_target_state,
)
from execution_runner_bindings import (
    run_local_validation,
    run_posix_ssh_validation,
    run_windows_ssh_validation,
    windows_validation_script,
)


EXECUTION_EXPORTS = (
    "remote_commit_error",
    "parse_progress_marker",
    "prepared_state_root",
    "should_reuse_prepared_state",
    "local_validation_command",
    "posix_ssh_validation_command",
    "validation_result_from_run",
    "validation_error_result",
    "unreachable_target_result",
    "target_exception_result",
    "completed_job_result",
    "sorted_target_results",
    "run_target_tasks",
    "run_logged_command",
    "run_local_validation",
    "run_posix_ssh_validation",
    "run_windows_ssh_validation",
    "windows_validation_script",
    "config_for_job_execution",
    "submission_target_state",
    "resolve_ssh_target_execution",
    "build_target_tasks",
    "process_job",
    "save_result",
    "print_result",
)


def heartbeat_interval_secs(bindings: Mapping[str, Any]) -> float:
    return _binding(bindings, "_execution").HEARTBEAT_INTERVAL_SECS


def stuck_idle_secs(bindings: Mapping[str, Any]) -> float:
    return _binding(bindings, "_execution").STUCK_IDLE_SECS


def remote_commit_error(bindings: Mapping[str, Any], target_name: str, host: str, job: dict) -> str:
    return _binding(bindings, "_execution").remote_commit_error(target_name, host, job)


def parse_progress_marker(bindings: Mapping[str, Any], line: str) -> dict:
    return _binding(bindings, "_execution").parse_progress_marker(line)


def prepared_state_root(bindings: Mapping[str, Any], target_name: str, validation: str) -> Path:
    return _binding(bindings, "_execution").prepared_state_root(target_name, validation)


def should_reuse_prepared_state(bindings: Mapping[str, Any], job: dict) -> bool:
    return _binding(bindings, "_execution").should_reuse_prepared_state(job)


def local_validation_command(bindings: Mapping[str, Any], job: dict, exclude_tests: str = "") -> tuple[list[str], str]:
    return _binding(bindings, "_execution").local_validation_command(job, exclude_tests)


def posix_ssh_validation_command(
    bindings: Mapping[str, Any],
    target_name: str,
    host: str,
    repo_path: str,
    job: dict,
    *,
    bundle_name: str,
    bundle_ref: str,
    exclude_tests: str = "",
) -> tuple[list[str], str]:
    return _binding(bindings, "_execution").posix_ssh_validation_command(
        target_name,
        host,
        repo_path,
        job,
        bundle_name=bundle_name,
        bundle_ref=bundle_ref,
        exclude_tests=exclude_tests,
    )


def validation_result_from_run(
    bindings: Mapping[str, Any],
    target_name: str,
    run: dict,
    *,
    log_path: Path,
    validation: str,
    transport_mode: str,
    timeout_secs: int = 3600,
) -> dict:
    return _binding(bindings, "_execution").validation_result_from_run(
        target_name,
        run,
        log_path=log_path,
        validation=validation,
        transport_mode=transport_mode,
        timeout_secs=timeout_secs,
    )


def validation_error_result(
    bindings: Mapping[str, Any],
    target_name: str,
    detail: str,
    *,
    log_path: Path,
    transport_mode: str,
) -> dict:
    return _binding(bindings, "_execution").validation_error_result(
        target_name,
        detail,
        log_path=log_path,
        transport_mode=transport_mode,
    )


def unreachable_target_result(bindings: Mapping[str, Any], target_name: str, detail: str = "Host unreachable") -> dict:
    return _binding(bindings, "_execution").unreachable_target_result(target_name, detail)


def target_exception_result(bindings: Mapping[str, Any], target_name: str, exc: Exception) -> dict:
    return _binding(bindings, "_execution").target_exception_result(target_name, exc)


def completed_job_result(bindings: Mapping[str, Any], job: dict, results: list[dict]) -> dict:
    return _binding(bindings, "_execution").completed_job_result(
        job,
        results,
        completed_at=_binding(bindings, "now_iso")(),
        provenance=_binding(bindings, "normalize_provenance")(job.get("provenance")),
    )


def sorted_target_results(bindings: Mapping[str, Any], results: list[dict]) -> list[dict]:
    return _binding(bindings, "_execution").sorted_target_results(results)


def run_target_tasks(
    bindings: Mapping[str, Any],
    tasks: list[tuple[str, Callable[[], dict]]],
    *,
    on_target_complete: Callable[[str, dict], None],
) -> list[dict]:
    return _binding(bindings, "_execution").run_target_tasks(
        tasks,
        exception_result_fn=_binding(bindings, "target_exception_result"),
        on_target_complete=on_target_complete,
    )


def run_logged_command(
    bindings: Mapping[str, Any],
    cmd: list[str],
    *,
    cwd: Path | None = None,
    input_text: str | None = None,
    timeout: int = 3600,
    log_path: Path | None = None,
    report_progress=None,
    heartbeat_interval_secs: float | None = None,
    stuck_idle_secs: float | None = None,
) -> dict:
    execution = _binding(bindings, "_execution")
    return execution.run_logged_command(
        cmd,
        cwd=cwd,
        input_text=input_text,
        timeout=timeout,
        log_path=log_path,
        report_progress=report_progress,
        heartbeat_interval_secs=execution.HEARTBEAT_INTERVAL_SECS
        if heartbeat_interval_secs is None
        else heartbeat_interval_secs,
        stuck_idle_secs=execution.STUCK_IDLE_SECS if stuck_idle_secs is None else stuck_idle_secs,
    )


def install_execution_helpers(bindings: dict[str, Any], names: tuple[str, ...] = EXECUTION_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
