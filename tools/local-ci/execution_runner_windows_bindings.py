"""Compatibility facade for Windows SSH validation runner bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from execution_runner_windows_run_bindings import (
    EXECUTION_RUNNER_WINDOWS_RUN_EXPORTS,
    install_execution_runner_windows_run_helpers,
    run_windows_ssh_validation,
)
from execution_runner_windows_script_bindings import (
    EXECUTION_RUNNER_WINDOWS_SCRIPT_EXPORTS,
    install_execution_runner_windows_script_helpers,
    windows_validation_script,
)


EXECUTION_RUNNER_WINDOWS_EXPORTS = (
    *EXECUTION_RUNNER_WINDOWS_RUN_EXPORTS,
    *EXECUTION_RUNNER_WINDOWS_SCRIPT_EXPORTS,
)


def install_execution_runner_windows_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_RUNNER_WINDOWS_EXPORTS,
) -> None:
    run_names = tuple(name for name in names if name in EXECUTION_RUNNER_WINDOWS_RUN_EXPORTS)
    script_names = tuple(name for name in names if name in EXECUTION_RUNNER_WINDOWS_SCRIPT_EXPORTS)
    known_names = set(EXECUTION_RUNNER_WINDOWS_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_execution_runner_windows_run_helpers(bindings, run_names)
    install_execution_runner_windows_script_helpers(bindings, script_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
