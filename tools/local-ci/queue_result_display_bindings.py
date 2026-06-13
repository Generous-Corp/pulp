"""Bindings from the local_ci facade to queue result display helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


QUEUE_RESULT_DISPLAY_EXPORTS = (
    "result_validation_line",
    "result_execution_line",
    "target_result_line",
    "result_target_lines",
    "result_overall_line",
)


def result_validation_line(bindings: Mapping[str, Any], result: dict) -> str | None:
    return _binding(bindings, "_queue_orchestrator").result_validation_line(result)


def result_execution_line(bindings: Mapping[str, Any], result: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").result_execution_line(result)


def target_result_line(bindings: Mapping[str, Any], item: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").target_result_line(item)


def result_target_lines(bindings: Mapping[str, Any], result: dict) -> list[str]:
    return _binding(bindings, "_queue_orchestrator").result_target_lines(result)


def result_overall_line(bindings: Mapping[str, Any], result: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").result_overall_line(result)
