"""Bindings from the local_ci facade to recent completed queue status helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


QUEUE_STATUS_RECENT_DISPLAY_EXPORTS = (
    "recent_completed_status_line",
    "recent_completed_missing_result_line",
)


def recent_completed_status_line(bindings: Mapping[str, Any], job: dict, result: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").recent_completed_status_line(job, result)


def recent_completed_missing_result_line(bindings: Mapping[str, Any], job: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").recent_completed_missing_result_line(job)
