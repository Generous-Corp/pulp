"""Bindings from the local_ci facade to queue log display helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


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
