"""Compatibility facade for validation target result helpers."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from execution_target_failure_result_bindings import (
    EXECUTION_TARGET_FAILURE_RESULT_EXPORTS,
    install_execution_target_failure_result_helpers,
    target_exception_result,
    unreachable_target_result,
)
from execution_target_run_result_bindings import (
    EXECUTION_TARGET_RUN_RESULT_EXPORTS,
    install_execution_target_run_result_helpers,
    validation_error_result,
    validation_result_from_run,
)


EXECUTION_TARGET_RESULT_EXPORTS = (
    *EXECUTION_TARGET_RUN_RESULT_EXPORTS,
    *EXECUTION_TARGET_FAILURE_RESULT_EXPORTS,
)


def install_execution_target_result_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_TARGET_RESULT_EXPORTS,
) -> None:
    run_names = tuple(name for name in names if name in EXECUTION_TARGET_RUN_RESULT_EXPORTS)
    failure_names = tuple(name for name in names if name in EXECUTION_TARGET_FAILURE_RESULT_EXPORTS)
    known_names = set(EXECUTION_TARGET_RESULT_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_execution_target_run_result_helpers(bindings, run_names)
    install_execution_target_failure_result_helpers(bindings, failure_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
