"""Compatibility facade for validation runner dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from execution_runner_local_bindings import (
    EXECUTION_RUNNER_LOCAL_EXPORTS,
    install_execution_runner_local_helpers,
    run_local_validation,
)
from execution_runner_ssh_bindings import (
    EXECUTION_RUNNER_SSH_EXPORTS,
    install_execution_runner_ssh_helpers,
    run_posix_ssh_validation,
)
from execution_runner_windows_bindings import (
    EXECUTION_RUNNER_WINDOWS_EXPORTS,
    install_execution_runner_windows_helpers,
    run_windows_ssh_validation,
    windows_validation_script,
)


EXECUTION_RUNNER_EXPORTS = (
    *EXECUTION_RUNNER_LOCAL_EXPORTS,
    *EXECUTION_RUNNER_SSH_EXPORTS,
    *EXECUTION_RUNNER_WINDOWS_EXPORTS,
)


def install_execution_runner_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_RUNNER_EXPORTS,
) -> None:
    local_names = tuple(name for name in names if name in EXECUTION_RUNNER_LOCAL_EXPORTS)
    ssh_names = tuple(name for name in names if name in EXECUTION_RUNNER_SSH_EXPORTS)
    windows_names = tuple(name for name in names if name in EXECUTION_RUNNER_WINDOWS_EXPORTS)
    known_names = set(EXECUTION_RUNNER_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_execution_runner_local_helpers(bindings, local_names)
    install_execution_runner_ssh_helpers(bindings, ssh_names)
    install_execution_runner_windows_helpers(bindings, windows_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
