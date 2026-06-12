"""Bindings from the local_ci facade to validation result helpers."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding


EXECUTION_RESULT_EXPORTS = (
    "validation_result_from_run",
    "validation_error_result",
    "unreachable_target_result",
    "target_exception_result",
    "completed_job_result",
    "sorted_target_results",
    "run_target_tasks",
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
