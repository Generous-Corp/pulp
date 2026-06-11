"""Bindings from the local_ci facade to utility command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def print_local_ci_state_footprint(bindings: Mapping[str, Any], *, indent: str = "") -> None:
    return _binding(bindings, "_cleanup_cli").print_local_ci_state_footprint(
        local_ci_state_footprint_fn=_binding(bindings, "local_ci_state_footprint"),
        state_footprint_lines_fn=_binding(bindings, "state_footprint_lines"),
        indent=indent,
    )


def print_local_ci_cleanup_plan(bindings: Mapping[str, Any], plan: dict, *, dry_run: bool) -> None:
    return _binding(bindings, "_cleanup_cli").print_local_ci_cleanup_plan(
        plan,
        dry_run=dry_run,
        cleanup_plan_lines_fn=_binding(bindings, "cleanup_plan_lines"),
    )


def cmd_cleanup(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cleanup_cli").cmd_cleanup(
        args,
        load_queue_fn=_binding(bindings, "load_queue"),
        collect_cleanup_plan_fn=_binding(bindings, "collect_local_ci_cleanup_plan"),
        apply_cleanup_plan_fn=_binding(bindings, "apply_local_ci_cleanup_plan"),
        print_cleanup_plan_fn=_binding(bindings, "print_local_ci_cleanup_plan"),
        print_state_footprint_fn=_binding(bindings, "print_local_ci_state_footprint"),
        format_size_fn=_binding(bindings, "format_size_bytes"),
        describe_path_fn=_binding(bindings, "describe_path_for_cleanup"),
    )


def cmd_bump(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_queue_commands_cli").cmd_bump(
        args,
        normalize_priority_fn=_binding(bindings, "normalize_priority"),
        bump_queue_command_job_fn=_binding(bindings, "bump_queue_command_job"),
        bump_queue_command_result_line_fn=_binding(bindings, "bump_queue_command_result_line"),
    )


def cmd_cancel(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_queue_commands_cli").cmd_cancel(
        args,
        cancel_queue_command_job_fn=_binding(bindings, "cancel_queue_command_job"),
        cancel_queue_command_result_line_fn=_binding(bindings, "cancel_queue_command_result_line"),
    )


def resolve_job_for_logs(bindings: Mapping[str, Any], job_ref: str | None) -> dict | None:
    return _binding(bindings, "_logs_cli").resolve_job_for_logs(
        job_ref,
        load_queue_fn=_binding(bindings, "load_queue"),
        current_runner_info_fn=_binding(bindings, "current_runner_info"),
        select_job_for_logs_fn=_binding(bindings, "_queue_orchestrator").select_job_for_logs,
    )


def cmd_logs(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_logs_cli").cmd_logs(
        args,
        resolve_job_for_logs_fn=_binding(bindings, "resolve_job_for_logs"),
        target_log_path_fn=_binding(bindings, "target_log_path"),
        job_logs_dir_fn=_binding(bindings, "job_logs_dir"),
        tail_lines_fn=_binding(bindings, "tail_lines"),
        missing_job_logs_line_fn=_binding(bindings, "missing_job_logs_line"),
        missing_log_files_line_fn=_binding(bindings, "missing_log_files_line"),
        job_logs_header_line_fn=_binding(bindings, "job_logs_header_line"),
        log_section_header_line_fn=_binding(bindings, "log_section_header_line"),
        empty_log_line_fn=_binding(bindings, "empty_log_line"),
    )


def cmd_evidence(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_evidence_cli").cmd_evidence(
        args,
        current_branch_fn=_binding(bindings, "current_branch"),
        evidence_scope_header_line_fn=_binding(bindings, "evidence_scope_header_line"),
        print_evidence_summary_fn=_binding(bindings, "print_evidence_summary"),
        evidence_empty_line_fn=_binding(bindings, "evidence_empty_line"),
    )
