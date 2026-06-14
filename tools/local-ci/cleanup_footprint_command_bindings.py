"""Facade bindings for cleanup state-footprint command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from cleanup_footprint_command_dependency_bindings import cleanup_footprint_command_dependencies


CLEANUP_FOOTPRINT_COMMAND_EXPORTS = (
    "print_local_ci_state_footprint",
)


def print_local_ci_state_footprint(bindings: Mapping[str, Any], *, indent: str = "") -> None:
    return _binding(bindings, "_cleanup_cli").print_local_ci_state_footprint(
        indent=indent,
        **cleanup_footprint_command_dependencies(bindings),
    )


def install_cleanup_footprint_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLEANUP_FOOTPRINT_COMMAND_EXPORTS,
) -> None:
    known_names = set(CLEANUP_FOOTPRINT_COMMAND_EXPORTS)
    footprint_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), footprint_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
