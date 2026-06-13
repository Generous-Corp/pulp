"""Compatibility facade for validation result dependency bindings."""

from __future__ import annotations

from binding_utils import install_local_helpers
from execution_completed_result_bindings import (
    completed_job_result,
    sorted_target_results,
)
from execution_target_result_bindings import (
    target_exception_result,
    unreachable_target_result,
    validation_error_result,
    validation_result_from_run,
)
from execution_task_result_bindings import run_target_tasks


EXECUTION_RESULT_EXPORTS = (
    "validation_result_from_run",
    "validation_error_result",
    "unreachable_target_result",
    "target_exception_result",
    "completed_job_result",
    "sorted_target_results",
    "run_target_tasks",
)


def install_execution_result_helpers(
    bindings: dict,
    names: tuple[str, ...] = EXECUTION_RESULT_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
