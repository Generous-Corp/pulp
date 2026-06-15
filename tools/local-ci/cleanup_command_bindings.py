"""Compatibility facade for cleanup command bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from cleanup_footprint_command_bindings import (
    CLEANUP_FOOTPRINT_COMMAND_EXPORTS,
    install_cleanup_footprint_command_helpers,
    print_local_ci_state_footprint,
)
from cleanup_plan_command_bindings import (
    CLEANUP_PLAN_COMMAND_EXPORTS,
    install_cleanup_plan_command_helpers,
    print_local_ci_cleanup_plan,
)
from cleanup_run_command_bindings import (
    CLEANUP_RUN_COMMAND_EXPORTS,
    cmd_cleanup,
    install_cleanup_run_command_helpers,
)


CLEANUP_COMMAND_EXPORTS = (
    *CLEANUP_FOOTPRINT_COMMAND_EXPORTS,
    *CLEANUP_PLAN_COMMAND_EXPORTS,
    *CLEANUP_RUN_COMMAND_EXPORTS,
)


def install_cleanup_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLEANUP_COMMAND_EXPORTS,
) -> None:
    known_names = set(CLEANUP_COMMAND_EXPORTS)
    footprint_names = tuple(name for name in names if name in CLEANUP_FOOTPRINT_COMMAND_EXPORTS)
    plan_names = tuple(name for name in names if name in CLEANUP_PLAN_COMMAND_EXPORTS)
    run_names = tuple(name for name in names if name in CLEANUP_RUN_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_cleanup_footprint_command_helpers(bindings, footprint_names)
    install_cleanup_plan_command_helpers(bindings, plan_names)
    install_cleanup_run_command_helpers(bindings, run_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
