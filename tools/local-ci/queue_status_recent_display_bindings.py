"""Bindings from the local_ci facade to recent completed queue status helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_STATUS_RECENT_DISPLAY_EXPORTS = (
    "recent_completed_status_line",
    "recent_completed_missing_result_line",
)


def recent_completed_status_line(bindings: Mapping[str, Any], job: dict, result: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").recent_completed_status_line(job, result)


def recent_completed_missing_result_line(bindings: Mapping[str, Any], job: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").recent_completed_missing_result_line(job)


def install_queue_status_recent_display_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_STATUS_RECENT_DISPLAY_EXPORTS,
) -> None:
    known_names = set(QUEUE_STATUS_RECENT_DISPLAY_EXPORTS)
    recent_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), recent_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
