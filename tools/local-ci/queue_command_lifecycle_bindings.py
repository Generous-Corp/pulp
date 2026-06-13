"""Compatibility composer for queue command lifecycle bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_command_mutation_bindings import (
    QUEUE_COMMAND_MUTATION_EXPORTS,
    bump_queue_command_job,
    cancel_queue_command_job,
    install_queue_command_mutation_helpers,
)
from queue_terminal_result_bindings import (
    QUEUE_TERMINAL_RESULT_EXPORTS,
    cancel_job_unlocked,
    install_queue_terminal_result_helpers,
    supersede_job_unlocked,
)


QUEUE_COMMAND_LIFECYCLE_EXPORTS = (
    *QUEUE_TERMINAL_RESULT_EXPORTS,
    *QUEUE_COMMAND_MUTATION_EXPORTS,
)


def install_queue_command_lifecycle_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_COMMAND_LIFECYCLE_EXPORTS,
) -> None:
    terminal_names = tuple(name for name in names if name in QUEUE_TERMINAL_RESULT_EXPORTS)
    command_names = tuple(name for name in names if name in QUEUE_COMMAND_MUTATION_EXPORTS)
    known_names = set(QUEUE_COMMAND_LIFECYCLE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_queue_terminal_result_helpers(bindings, terminal_names)
    install_queue_command_mutation_helpers(bindings, command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
