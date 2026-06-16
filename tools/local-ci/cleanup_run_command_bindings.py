"""Facade bindings for cleanup command execution."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from cleanup_run_command_dependency_bindings import cleanup_run_command_dependencies


CLEANUP_RUN_COMMAND_EXPORTS = (
    "cmd_cleanup",
)


def cmd_cleanup(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cleanup_cli").cmd_cleanup(
        args,
        **cleanup_run_command_dependencies(bindings),
    )


def install_cleanup_run_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLEANUP_RUN_COMMAND_EXPORTS,
) -> None:
    known_names = set(CLEANUP_RUN_COMMAND_EXPORTS)
    run_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), run_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
