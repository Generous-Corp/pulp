"""Facade bindings for queue bump utility command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from utility_queue_bump_command_dependency_bindings import utility_queue_bump_command_dependencies


UTILITY_QUEUE_BUMP_COMMAND_EXPORTS = (
    "cmd_bump",
)


def cmd_bump(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_queue_commands_cli").cmd_bump(
        args,
        **utility_queue_bump_command_dependencies(bindings),
    )


def install_utility_queue_bump_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = UTILITY_QUEUE_BUMP_COMMAND_EXPORTS,
) -> None:
    known_names = set(UTILITY_QUEUE_BUMP_COMMAND_EXPORTS)
    bump_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), bump_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
