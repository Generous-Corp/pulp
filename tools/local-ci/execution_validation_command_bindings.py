"""Compatibility facade for validation command construction bindings."""

from __future__ import annotations

from binding_utils import install_local_helpers
from execution_local_command_bindings import (
    EXECUTION_LOCAL_COMMAND_EXPORTS,
    install_execution_local_command_helpers,
    local_validation_command,
)
from execution_posix_command_bindings import (
    EXECUTION_POSIX_COMMAND_EXPORTS,
    install_execution_posix_command_helpers,
    posix_ssh_validation_command,
)
from execution_windows_command_bindings import (
    EXECUTION_WINDOWS_COMMAND_EXPORTS,
    install_execution_windows_command_helpers,
    windows_validation_script,
)


EXECUTION_VALIDATION_COMMAND_EXPORTS = (
    *EXECUTION_LOCAL_COMMAND_EXPORTS,
    *EXECUTION_POSIX_COMMAND_EXPORTS,
    *EXECUTION_WINDOWS_COMMAND_EXPORTS,
)


def install_execution_validation_command_helpers(
    bindings: dict,
    names: tuple[str, ...] = EXECUTION_VALIDATION_COMMAND_EXPORTS,
) -> None:
    local_names = tuple(name for name in names if name in EXECUTION_LOCAL_COMMAND_EXPORTS)
    posix_names = tuple(name for name in names if name in EXECUTION_POSIX_COMMAND_EXPORTS)
    windows_names = tuple(name for name in names if name in EXECUTION_WINDOWS_COMMAND_EXPORTS)
    known_names = set(EXECUTION_VALIDATION_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_execution_local_command_helpers(bindings, local_names)
    install_execution_posix_command_helpers(bindings, posix_names)
    install_execution_windows_command_helpers(bindings, windows_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
