"""Compatibility facade for validation job orchestration dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from execution_job_config_bindings import (
    EXECUTION_JOB_CONFIG_EXPORTS,
    config_for_job_execution,
    install_execution_job_config_helpers,
    resolve_ssh_target_execution,
    submission_target_state,
)
from execution_result_io_bindings import (
    EXECUTION_RESULT_IO_EXPORTS,
    install_execution_result_io_helpers,
    print_result,
    save_result,
)
from execution_target_task_bindings import (
    EXECUTION_TARGET_TASK_EXPORTS,
    build_target_tasks,
    install_execution_target_task_helpers,
    process_job,
)


EXECUTION_JOB_EXPORTS = (
    *EXECUTION_JOB_CONFIG_EXPORTS,
    *EXECUTION_TARGET_TASK_EXPORTS,
    *EXECUTION_RESULT_IO_EXPORTS,
)


def install_execution_job_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_JOB_EXPORTS,
) -> None:
    config_names = tuple(name for name in names if name in EXECUTION_JOB_CONFIG_EXPORTS)
    task_names = tuple(name for name in names if name in EXECUTION_TARGET_TASK_EXPORTS)
    result_io_names = tuple(name for name in names if name in EXECUTION_RESULT_IO_EXPORTS)
    known_names = set(EXECUTION_JOB_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_execution_job_config_helpers(bindings, config_names)
    install_execution_target_task_helpers(bindings, task_names)
    install_execution_result_io_helpers(bindings, result_io_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
