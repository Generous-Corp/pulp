"""Bindings from the local_ci facade to validation target-task result helpers."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


EXECUTION_TASK_RESULT_EXPORTS = ("run_target_tasks",)


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


def install_execution_task_result_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_TASK_RESULT_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
