"""Bindings from the local_ci facade to queue display helpers."""

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
