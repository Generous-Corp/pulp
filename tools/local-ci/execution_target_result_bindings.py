"""Bindings from the local_ci facade to validation target result helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


EXECUTION_TARGET_RESULT_EXPORTS = (
    "validation_result_from_run",
    "validation_error_result",
    "unreachable_target_result",
    "target_exception_result",
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


def install_execution_target_result_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_TARGET_RESULT_EXPORTS,
) -> None:
    known_names = set(EXECUTION_TARGET_RESULT_EXPORTS)
    result_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), result_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
