"""Bindings from the local_ci facade to validation target failure result helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


EXECUTION_TARGET_FAILURE_RESULT_EXPORTS = (
    "unreachable_target_result",
    "target_exception_result",
)


def unreachable_target_result(bindings: Mapping[str, Any], target_name: str, detail: str = "Host unreachable") -> dict:
    return _binding(bindings, "_execution").unreachable_target_result(target_name, detail)


def target_exception_result(bindings: Mapping[str, Any], target_name: str, exc: Exception) -> dict:
    return _binding(bindings, "_execution").target_exception_result(target_name, exc)


def install_execution_target_failure_result_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_TARGET_FAILURE_RESULT_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
