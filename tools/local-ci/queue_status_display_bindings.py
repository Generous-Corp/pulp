"""Compatibility facade for queue status display dependency bindings."""

from __future__ import annotations

from queue_status_active_display_bindings import (
    QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS,
    status_active_targets,
    status_runner_line,
    summarize_active_targets,
)
from queue_status_recent_display_bindings import (
    QUEUE_STATUS_RECENT_DISPLAY_EXPORTS,
    recent_completed_missing_result_line,
    recent_completed_status_line,
)
from queue_status_target_display_bindings import (
    QUEUE_STATUS_TARGET_DISPLAY_EXPORTS,
    status_submission_lines,
    status_target_detail_lines,
    status_target_states,
    target_state_detail_parts,
)


QUEUE_STATUS_DISPLAY_EXPORTS = (
    *QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS[:2],
    *QUEUE_STATUS_TARGET_DISPLAY_EXPORTS,
    QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS[2],
    *QUEUE_STATUS_RECENT_DISPLAY_EXPORTS,
)
