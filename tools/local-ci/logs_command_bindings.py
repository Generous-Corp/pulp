"""Compatibility facade for logs command bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from logs_resolution_command_bindings import (
    LOGS_RESOLUTION_COMMAND_EXPORTS,
    install_logs_resolution_command_helpers,
    resolve_job_for_logs,
)
from logs_run_command_bindings import (
    LOGS_RUN_COMMAND_EXPORTS,
    cmd_logs,
    install_logs_run_command_helpers,
)


LOGS_COMMAND_EXPORTS = (
    *LOGS_RESOLUTION_COMMAND_EXPORTS,
    *LOGS_RUN_COMMAND_EXPORTS,
)


def install_logs_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LOGS_COMMAND_EXPORTS,
) -> None:
    known_names = set(LOGS_COMMAND_EXPORTS)
    resolution_names = tuple(name for name in names if name in LOGS_RESOLUTION_COMMAND_EXPORTS)
    run_names = tuple(name for name in names if name in LOGS_RUN_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_logs_resolution_command_helpers(bindings, resolution_names)
    install_logs_run_command_helpers(bindings, run_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
