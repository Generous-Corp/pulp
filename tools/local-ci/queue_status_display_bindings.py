"""Compatibility facade for queue status display dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_status_active_display_bindings import (
    QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS,
    install_queue_status_active_display_helpers,
    status_active_targets,
    status_runner_line,
    summarize_active_targets,
)
from queue_status_recent_display_bindings import (
    QUEUE_STATUS_RECENT_DISPLAY_EXPORTS,
    install_queue_status_recent_display_helpers,
    recent_completed_missing_result_line,
    recent_completed_status_line,
)
from queue_status_target_display_bindings import (
    QUEUE_STATUS_TARGET_DISPLAY_EXPORTS,
    install_queue_status_target_display_helpers,
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


def install_queue_status_display_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_STATUS_DISPLAY_EXPORTS,
) -> None:
    active_names = tuple(name for name in names if name in QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS)
    target_names = tuple(name for name in names if name in QUEUE_STATUS_TARGET_DISPLAY_EXPORTS)
    recent_names = tuple(name for name in names if name in QUEUE_STATUS_RECENT_DISPLAY_EXPORTS)
    known_names = set(QUEUE_STATUS_DISPLAY_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_queue_status_active_display_helpers(bindings, active_names)
    install_queue_status_target_display_helpers(bindings, target_names)
    install_queue_status_recent_display_helpers(bindings, recent_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
