"""Compatibility facade for queue utility command bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from utility_queue_bump_command_bindings import (
    UTILITY_QUEUE_BUMP_COMMAND_EXPORTS,
    cmd_bump,
    install_utility_queue_bump_command_helpers,
)
from utility_queue_cancel_command_bindings import (
    UTILITY_QUEUE_CANCEL_COMMAND_EXPORTS,
    cmd_cancel,
    install_utility_queue_cancel_command_helpers,
)


UTILITY_QUEUE_COMMAND_EXPORTS = (
    *UTILITY_QUEUE_BUMP_COMMAND_EXPORTS,
    *UTILITY_QUEUE_CANCEL_COMMAND_EXPORTS,
)


def install_utility_queue_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = UTILITY_QUEUE_COMMAND_EXPORTS,
) -> None:
    known_names = set(UTILITY_QUEUE_COMMAND_EXPORTS)
    bump_names = tuple(name for name in names if name in UTILITY_QUEUE_BUMP_COMMAND_EXPORTS)
    cancel_names = tuple(name for name in names if name in UTILITY_QUEUE_CANCEL_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_utility_queue_bump_command_helpers(bindings, bump_names)
    install_utility_queue_cancel_command_helpers(bindings, cancel_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
