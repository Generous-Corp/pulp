"""Compatibility facade for validation execution dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from execution_command_bindings import (
    EXECUTION_COMMAND_EXPORTS,
    local_validation_command,
    posix_ssh_validation_command,
    prepared_state_root,
    remote_commit_error,
    should_reuse_prepared_state,
    windows_validation_script,
)
from execution_job_bindings import (
    build_target_tasks,
    config_for_job_execution,
    print_result,
    process_job,
    resolve_ssh_target_execution,
    save_result,
    submission_target_state,
)
from execution_logging_bindings import (
    EXECUTION_LOGGING_EXPORTS,
    heartbeat_interval_secs,
    parse_progress_marker,
    run_logged_command,
    stuck_idle_secs,
)
from execution_result_bindings import (
    EXECUTION_RESULT_EXPORTS,
    completed_job_result,
    run_target_tasks,
    sorted_target_results,
    target_exception_result,
    unreachable_target_result,
    validation_error_result,
    validation_result_from_run,
)
from execution_runner_bindings import (
    run_local_validation,
    run_posix_ssh_validation,
    run_windows_ssh_validation,
)


EXECUTION_EXPORTS = (
    *EXECUTION_COMMAND_EXPORTS,
    *EXECUTION_RESULT_EXPORTS,
    *EXECUTION_LOGGING_EXPORTS,
    "run_local_validation",
    "run_posix_ssh_validation",
    "run_windows_ssh_validation",
    "config_for_job_execution",
    "submission_target_state",
    "resolve_ssh_target_execution",
    "build_target_tasks",
    "process_job",
    "save_result",
    "print_result",
)


def install_execution_helpers(bindings: dict[str, Any], names: tuple[str, ...] = EXECUTION_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
