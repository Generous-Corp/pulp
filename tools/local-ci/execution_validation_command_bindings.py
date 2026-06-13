"""Compatibility facade for validation command construction bindings."""

from __future__ import annotations

from execution_local_command_bindings import EXECUTION_LOCAL_COMMAND_EXPORTS, local_validation_command
from execution_posix_command_bindings import EXECUTION_POSIX_COMMAND_EXPORTS, posix_ssh_validation_command
from execution_windows_command_bindings import EXECUTION_WINDOWS_COMMAND_EXPORTS, windows_validation_script


EXECUTION_VALIDATION_COMMAND_EXPORTS = (
    *EXECUTION_LOCAL_COMMAND_EXPORTS,
    *EXECUTION_POSIX_COMMAND_EXPORTS,
    *EXECUTION_WINDOWS_COMMAND_EXPORTS,
)
