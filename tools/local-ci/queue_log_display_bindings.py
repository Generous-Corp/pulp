"""Bindings from the local_ci facade to queue log display helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_LOG_DISPLAY_EXPORTS = (
    "missing_job_logs_line",
    "missing_log_files_line",
    "job_logs_header_line",
    "log_section_header_line",
    "empty_log_line",
)


def missing_job_logs_line(bindings: Mapping[str, Any]) -> str:
    return _binding(bindings, "_queue_orchestrator").missing_job_logs_line()


def missing_log_files_line(bindings: Mapping[str, Any], job: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").missing_log_files_line(job)


def job_logs_header_line(bindings: Mapping[str, Any], job: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").job_logs_header_line(job)


def log_section_header_line(bindings: Mapping[str, Any], target: str) -> str:
    return _binding(bindings, "_queue_orchestrator").log_section_header_line(target)


def empty_log_line(bindings: Mapping[str, Any]) -> str:
    return _binding(bindings, "_queue_orchestrator").empty_log_line()


def install_queue_log_display_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_LOG_DISPLAY_EXPORTS,
) -> None:
    known_names = set(QUEUE_LOG_DISPLAY_EXPORTS)
    log_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), log_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
