"""Compatibility facade for validation command dependency bindings."""

from __future__ import annotations

from binding_utils import install_local_helpers
from execution_command_state_bindings import (
    EXECUTION_COMMAND_STATE_EXPORTS,
    install_execution_command_state_helpers,
    prepared_state_root,
    remote_commit_error,
    should_reuse_prepared_state,
)
from execution_validation_command_bindings import (
    EXECUTION_VALIDATION_COMMAND_EXPORTS,
    install_execution_validation_command_helpers,
    local_validation_command,
    posix_ssh_validation_command,
    windows_validation_script,
)


EXECUTION_COMMAND_EXPORTS = (
    *EXECUTION_COMMAND_STATE_EXPORTS,
    *EXECUTION_VALIDATION_COMMAND_EXPORTS,
)


def install_execution_command_helpers(
    bindings: dict,
    names: tuple[str, ...] = EXECUTION_COMMAND_EXPORTS,
) -> None:
    state_names = tuple(name for name in names if name in EXECUTION_COMMAND_STATE_EXPORTS)
    validation_names = tuple(name for name in names if name in EXECUTION_VALIDATION_COMMAND_EXPORTS)
    known_names = set(EXECUTION_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_execution_command_state_helpers(bindings, state_names)
    install_execution_validation_command_helpers(bindings, validation_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
