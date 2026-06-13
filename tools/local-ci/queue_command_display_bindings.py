"""Bindings from the local_ci facade to queue command display helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def summarize_job(bindings: Mapping[str, Any], job: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").summarize_job(job)


def bump_queue_command_result_line(bindings: Mapping[str, Any], result: dict, job_ref: str) -> tuple[int, str]:
    return _binding(bindings, "_queue_orchestrator").bump_queue_command_result_line(result, job_ref)


def cancel_queue_command_result_line(bindings: Mapping[str, Any], result: dict, job_ref: str) -> tuple[int, str]:
    return _binding(bindings, "_queue_orchestrator").cancel_queue_command_result_line(result, job_ref)


def enqueue_command_result_line(bindings: Mapping[str, Any], job: dict, *, created: bool) -> str:
    return _binding(bindings, "_queue_orchestrator").enqueue_command_result_line(job, created=created)


def drain_runner_active_line(bindings: Mapping[str, Any], runner_info: dict | None) -> str:
    return _binding(bindings, "_queue_orchestrator").drain_runner_active_line(runner_info)
