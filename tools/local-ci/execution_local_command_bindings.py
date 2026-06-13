"""Bindings from the local_ci facade to local validation command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


EXECUTION_LOCAL_COMMAND_EXPORTS = ("local_validation_command",)


def local_validation_command(bindings: Mapping[str, Any], job: dict, exclude_tests: str = "") -> tuple[list[str], str]:
    return _binding(bindings, "_execution").local_validation_command(job, exclude_tests)


def install_execution_local_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_LOCAL_COMMAND_EXPORTS,
) -> None:
    known_names = set(EXECUTION_LOCAL_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
