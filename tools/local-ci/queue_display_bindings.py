"""Compatibility facade for queue display dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_command_display_bindings import (
    QUEUE_COMMAND_DISPLAY_EXPORTS,
    bump_queue_command_result_line,
    cancel_queue_command_result_line,
    drain_runner_active_line,
    enqueue_command_result_line,
    summarize_job,
)
from queue_log_display_bindings import (
    QUEUE_LOG_DISPLAY_EXPORTS,
    empty_log_line,
    job_logs_header_line,
    log_section_header_line,
    missing_job_logs_line,
    missing_log_files_line,
)
from queue_result_display_bindings import (
    QUEUE_RESULT_DISPLAY_EXPORTS,
    result_execution_line,
    result_overall_line,
    result_target_lines,
    result_validation_line,
    target_result_line,
)
from queue_status_display_bindings import (
    QUEUE_STATUS_DISPLAY_EXPORTS,
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
    *QUEUE_COMMAND_DISPLAY_EXPORTS,
    *QUEUE_STATUS_DISPLAY_EXPORTS,
    *QUEUE_RESULT_DISPLAY_EXPORTS,
    *QUEUE_LOG_DISPLAY_EXPORTS,
)


def install_queue_display_helpers(bindings: dict[str, Any], names: tuple[str, ...] = QUEUE_DISPLAY_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
