"""Compatibility installer for local_ci utility command facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from cleanup_command_bindings import (
    CLEANUP_COMMAND_EXPORTS,
    cmd_cleanup,
    install_cleanup_command_helpers,
    print_local_ci_cleanup_plan,
    print_local_ci_state_footprint,
)
from evidence_command_bindings import EVIDENCE_COMMAND_EXPORTS, cmd_evidence, install_evidence_command_helpers
from logs_command_bindings import (
    LOGS_COMMAND_EXPORTS,
    cmd_logs,
    install_logs_command_helpers,
    resolve_job_for_logs,
)
from utility_queue_command_bindings import (
    UTILITY_QUEUE_COMMAND_EXPORTS,
    cmd_bump,
    cmd_cancel,
    install_utility_queue_command_helpers,
)


UTILITY_COMMAND_EXPORTS = (
    *CLEANUP_COMMAND_EXPORTS,
    *UTILITY_QUEUE_COMMAND_EXPORTS,
    *LOGS_COMMAND_EXPORTS,
    *EVIDENCE_COMMAND_EXPORTS,
)


def install_utility_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = UTILITY_COMMAND_EXPORTS,
) -> None:
    cleanup_names = tuple(name for name in names if name in CLEANUP_COMMAND_EXPORTS)
    queue_names = tuple(name for name in names if name in UTILITY_QUEUE_COMMAND_EXPORTS)
    logs_names = tuple(name for name in names if name in LOGS_COMMAND_EXPORTS)
    evidence_names = tuple(name for name in names if name in EVIDENCE_COMMAND_EXPORTS)
    known_names = set(UTILITY_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_cleanup_command_helpers(bindings, cleanup_names)
    install_utility_queue_command_helpers(bindings, queue_names)
    install_logs_command_helpers(bindings, logs_names)
    install_evidence_command_helpers(bindings, evidence_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
