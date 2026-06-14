"""Bindings from the local_ci facade to active queue status display helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS = (
    "summarize_active_targets",
    "status_active_targets",
    "status_runner_line",
)


def summarize_active_targets(
    bindings: Mapping[str, Any],
    active_targets: dict | None,
    preferred_order: list[str] | None = None,
) -> str:
    return _binding(bindings, "_queue_orchestrator").summarize_active_targets(active_targets, preferred_order)


def status_active_targets(bindings: Mapping[str, Any], job: dict, runner_info: dict | None = None) -> dict | None:
    return _binding(bindings, "_queue_orchestrator").status_active_targets(job, runner_info)


def status_runner_line(bindings: Mapping[str, Any], runner_info: dict | None) -> str:
    return _binding(bindings, "_queue_orchestrator").status_runner_line(runner_info)


def install_queue_status_active_display_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS,
) -> None:
    known_names = set(QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS)
    active_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), active_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
