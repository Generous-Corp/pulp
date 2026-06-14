"""Facade bindings for cleanup plan command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CLEANUP_PLAN_COMMAND_EXPORTS = (
    "print_local_ci_cleanup_plan",
)


def print_local_ci_cleanup_plan(bindings: Mapping[str, Any], plan: dict, *, dry_run: bool) -> None:
    return _binding(bindings, "_cleanup_cli").print_local_ci_cleanup_plan(
        plan,
        dry_run=dry_run,
        cleanup_plan_lines_fn=_binding(bindings, "cleanup_plan_lines"),
    )


def install_cleanup_plan_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLEANUP_PLAN_COMMAND_EXPORTS,
) -> None:
    known_names = set(CLEANUP_PLAN_COMMAND_EXPORTS)
    plan_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), plan_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
