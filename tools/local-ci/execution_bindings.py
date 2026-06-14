"""Compatibility facade for validation execution dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from execution_command_bindings import (
    EXECUTION_COMMAND_EXPORTS,
    install_execution_command_helpers,
    local_validation_command,
    posix_ssh_validation_command,
    prepared_state_root,
    remote_commit_error,
    should_reuse_prepared_state,
    windows_validation_script,
)
from execution_job_bindings import (
    EXECUTION_JOB_EXPORTS,
    build_target_tasks,
    config_for_job_execution,
    install_execution_job_helpers,
    print_result,
    process_job,
    resolve_ssh_target_execution,
    save_result,
    submission_target_state,
)
from execution_logging_bindings import (
    EXECUTION_LOGGING_EXPORTS,
    heartbeat_interval_secs,
    install_execution_logging_helpers,
    parse_progress_marker,
    run_logged_command,
    stuck_idle_secs,
)
from execution_result_bindings import (
    EXECUTION_RESULT_EXPORTS,
    completed_job_result,
    install_execution_result_helpers,
    run_target_tasks,
    sorted_target_results,
    target_exception_result,
    unreachable_target_result,
    validation_error_result,
    validation_result_from_run,
)
from execution_runner_bindings import (
    EXECUTION_RUNNER_EXPORTS,
    install_execution_runner_helpers,
    run_local_validation,
    run_posix_ssh_validation,
    run_windows_ssh_validation,
)


EXECUTION_RUNNER_INSTALL_EXPORTS = (
    "run_local_validation",
    "run_posix_ssh_validation",
    "run_windows_ssh_validation",
)

EXECUTION_EXPORTS = (
    *EXECUTION_COMMAND_EXPORTS,
    *EXECUTION_RESULT_EXPORTS,
    *EXECUTION_LOGGING_EXPORTS,
    *EXECUTION_RUNNER_INSTALL_EXPORTS,
    *EXECUTION_JOB_EXPORTS,
)


def install_execution_helpers(bindings: dict[str, Any], names: tuple[str, ...] = EXECUTION_EXPORTS) -> None:
    command_names = tuple(name for name in names if name in EXECUTION_COMMAND_EXPORTS)
    result_names = tuple(name for name in names if name in EXECUTION_RESULT_EXPORTS)
    logging_names = tuple(name for name in names if name in EXECUTION_LOGGING_EXPORTS)
    runner_names = tuple(name for name in names if name in EXECUTION_RUNNER_INSTALL_EXPORTS)
    job_names = tuple(name for name in names if name in EXECUTION_JOB_EXPORTS)
    known_names = (
        set(EXECUTION_COMMAND_EXPORTS)
        | set(EXECUTION_RESULT_EXPORTS)
        | set(EXECUTION_LOGGING_EXPORTS)
        | set(EXECUTION_RUNNER_INSTALL_EXPORTS)
        | set(EXECUTION_JOB_EXPORTS)
    )
    unknown_names = tuple(name for name in names if name not in known_names)

    install_execution_command_helpers(bindings, command_names)
    install_execution_result_helpers(bindings, result_names)
    install_execution_logging_helpers(bindings, logging_names)
    install_execution_runner_helpers(bindings, runner_names)
    install_execution_job_helpers(bindings, job_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
