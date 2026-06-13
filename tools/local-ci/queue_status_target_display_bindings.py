"""Bindings from the local_ci facade to queue target-status display helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


QUEUE_STATUS_TARGET_DISPLAY_EXPORTS = (
    "status_target_states",
    "status_submission_lines",
    "target_state_detail_parts",
    "status_target_detail_lines",
)


def status_target_states(bindings: Mapping[str, Any], job: dict, active_targets: dict | None) -> list[tuple[str, dict]]:
    return _binding(bindings, "_queue_orchestrator").status_target_states(job, active_targets)


def status_submission_lines(bindings: Mapping[str, Any], job: dict) -> list[str]:
    return _binding(bindings, "_queue_orchestrator").status_submission_lines(job)


def target_state_detail_parts(bindings: Mapping[str, Any], state: dict) -> list[str]:
    return _binding(bindings, "_queue_orchestrator").target_state_detail_parts(state)


def status_target_detail_lines(bindings: Mapping[str, Any], job: dict, active_targets: dict | None) -> list[str]:
    return _binding(bindings, "_queue_orchestrator").status_target_detail_lines(job, active_targets)
