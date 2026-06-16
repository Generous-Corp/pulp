"""Facade bindings for cleanup plan apply and display helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from cleanup_plan_apply_dependency_bindings import cleanup_plan_lines_dependencies


CLEANUP_PLAN_APPLY_EXPORTS = (
    "apply_local_ci_cleanup_plan",
    "cleanup_plan_lines",
)


def apply_local_ci_cleanup_plan(bindings: Mapping[str, Any], plan: dict) -> dict:
    return _binding(bindings, "_cleanup").apply_local_ci_cleanup_plan(plan)


def cleanup_plan_lines(bindings: Mapping[str, Any], plan: dict, *, dry_run: bool) -> list[str]:
    return _binding(bindings, "_cleanup").cleanup_plan_lines(
        plan,
        dry_run=dry_run,
        **cleanup_plan_lines_dependencies(bindings),
    )


def install_cleanup_plan_apply_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLEANUP_PLAN_APPLY_EXPORTS,
) -> None:
    known_names = set(CLEANUP_PLAN_APPLY_EXPORTS)
    apply_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), apply_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
