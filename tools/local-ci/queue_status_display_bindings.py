"""Bindings from the local_ci facade to queue status display helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


QUEUE_STATUS_DISPLAY_EXPORTS = (
    "summarize_active_targets",
    "status_active_targets",
    "status_target_states",
    "status_submission_lines",
    "target_state_detail_parts",
    "status_target_detail_lines",
    "status_runner_line",
    "recent_completed_status_line",
    "recent_completed_missing_result_line",
)


def summarize_active_targets(
    bindings: Mapping[str, Any],
    active_targets: dict | None,
    preferred_order: list[str] | None = None,
) -> str:
    return _binding(bindings, "_queue_orchestrator").summarize_active_targets(active_targets, preferred_order)


def status_active_targets(bindings: Mapping[str, Any], job: dict, runner_info: dict | None = None) -> dict | None:
    return _binding(bindings, "_queue_orchestrator").status_active_targets(job, runner_info)


def status_target_states(bindings: Mapping[str, Any], job: dict, active_targets: dict | None) -> list[tuple[str, dict]]:
    return _binding(bindings, "_queue_orchestrator").status_target_states(job, active_targets)


def status_submission_lines(bindings: Mapping[str, Any], job: dict) -> list[str]:
    return _binding(bindings, "_queue_orchestrator").status_submission_lines(job)


def target_state_detail_parts(bindings: Mapping[str, Any], state: dict) -> list[str]:
    return _binding(bindings, "_queue_orchestrator").target_state_detail_parts(state)


def status_target_detail_lines(bindings: Mapping[str, Any], job: dict, active_targets: dict | None) -> list[str]:
    return _binding(bindings, "_queue_orchestrator").status_target_detail_lines(job, active_targets)


def status_runner_line(bindings: Mapping[str, Any], runner_info: dict | None) -> str:
    return _binding(bindings, "_queue_orchestrator").status_runner_line(runner_info)


def recent_completed_status_line(bindings: Mapping[str, Any], job: dict, result: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").recent_completed_status_line(job, result)


def recent_completed_missing_result_line(bindings: Mapping[str, Any], job: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").recent_completed_missing_result_line(job)
