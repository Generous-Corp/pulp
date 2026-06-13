"""Compatibility facade for validation logging/progress dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from execution_logged_command_bindings import (
    EXECUTION_LOGGED_COMMAND_EXPORTS,
    install_execution_logged_command_helpers,
    run_logged_command,
)
from execution_logging_timing_bindings import (
    EXECUTION_LOGGING_TIMING_EXPORTS,
    heartbeat_interval_secs,
    install_execution_logging_timing_helpers,
    stuck_idle_secs,
)
from execution_progress_marker_bindings import (
    EXECUTION_PROGRESS_MARKER_EXPORTS,
    install_execution_progress_marker_helpers,
    parse_progress_marker,
)


EXECUTION_LOGGING_EXPORTS = (
    *EXECUTION_LOGGING_TIMING_EXPORTS,
    *EXECUTION_PROGRESS_MARKER_EXPORTS,
    *EXECUTION_LOGGED_COMMAND_EXPORTS,
)


def install_execution_logging_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_LOGGING_EXPORTS,
) -> None:
    timing_names = tuple(name for name in names if name in EXECUTION_LOGGING_TIMING_EXPORTS)
    progress_names = tuple(name for name in names if name in EXECUTION_PROGRESS_MARKER_EXPORTS)
    command_names = tuple(name for name in names if name in EXECUTION_LOGGED_COMMAND_EXPORTS)
    known_names = set(EXECUTION_LOGGING_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_execution_logging_timing_helpers(bindings, timing_names)
    install_execution_progress_marker_helpers(bindings, progress_names)
    install_execution_logged_command_helpers(bindings, command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
