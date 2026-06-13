"""Compatibility facade for queue display dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_command_display_bindings import (
    bump_queue_command_result_line,
    cancel_queue_command_result_line,
    drain_runner_active_line,
    enqueue_command_result_line,
    summarize_job,
)
from queue_log_display_bindings import (
    empty_log_line,
    job_logs_header_line,
    log_section_header_line,
    missing_job_logs_line,
    missing_log_files_line,
)
from queue_result_display_bindings import (
    result_execution_line,
    result_overall_line,
    result_target_lines,
    result_validation_line,
    target_result_line,
)
from queue_status_display_bindings import (
    recent_completed_missing_result_line,
    recent_completed_status_line,
    status_active_targets,
    status_runner_line,
    status_submission_lines,
    status_target_detail_lines,
    status_target_states,
    summarize_active_targets,
    target_state_detail_parts,
)


QUEUE_DISPLAY_EXPORTS = (
    "summarize_job",
    "bump_queue_command_result_line",
    "cancel_queue_command_result_line",
    "enqueue_command_result_line",
    "drain_runner_active_line",
    "summarize_active_targets",
    "status_active_targets",
    "status_target_states",
    "status_submission_lines",
    "target_state_detail_parts",
    "status_target_detail_lines",
    "status_runner_line",
    "recent_completed_status_line",
    "recent_completed_missing_result_line",
    "result_validation_line",
    "result_execution_line",
    "target_result_line",
    "result_target_lines",
    "result_overall_line",
    "missing_job_logs_line",
    "missing_log_files_line",
    "job_logs_header_line",
    "log_section_header_line",
    "empty_log_line",
)


def install_queue_display_helpers(bindings: dict[str, Any], names: tuple[str, ...] = QUEUE_DISPLAY_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
