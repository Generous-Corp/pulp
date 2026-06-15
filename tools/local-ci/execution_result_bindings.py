"""Compatibility facade for validation result dependency bindings."""

from __future__ import annotations

from binding_utils import install_local_helpers
from execution_completed_result_bindings import (
    EXECUTION_COMPLETED_RESULT_EXPORTS,
    completed_job_result,
    install_execution_completed_result_helpers,
    sorted_target_results,
)
from execution_target_result_bindings import (
    EXECUTION_TARGET_RESULT_EXPORTS,
    install_execution_target_result_helpers,
    target_exception_result,
    unreachable_target_result,
    validation_error_result,
    validation_result_from_run,
)
from execution_task_result_bindings import (
    EXECUTION_TASK_RESULT_EXPORTS,
    install_execution_task_result_helpers,
    run_target_tasks,
)


EXECUTION_RESULT_EXPORTS = (
    *EXECUTION_TARGET_RESULT_EXPORTS,
    *EXECUTION_COMPLETED_RESULT_EXPORTS,
    *EXECUTION_TASK_RESULT_EXPORTS,
)


def install_execution_result_helpers(
    bindings: dict,
    names: tuple[str, ...] = EXECUTION_RESULT_EXPORTS,
) -> None:
    target_names = tuple(name for name in names if name in EXECUTION_TARGET_RESULT_EXPORTS)
    completed_names = tuple(name for name in names if name in EXECUTION_COMPLETED_RESULT_EXPORTS)
    task_names = tuple(name for name in names if name in EXECUTION_TASK_RESULT_EXPORTS)
    known_names = set(EXECUTION_RESULT_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_execution_target_result_helpers(bindings, target_names)
    install_execution_completed_result_helpers(bindings, completed_names)
    install_execution_task_result_helpers(bindings, task_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
