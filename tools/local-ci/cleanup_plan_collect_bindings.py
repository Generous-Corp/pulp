"""Facade bindings for cleanup plan collection helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from cleanup_plan_collect_dependency_bindings import (
    cleanup_plan_collect_dependencies,
    cleanup_plan_retention_values,
)


CLEANUP_PLAN_COLLECT_EXPORTS = (
    "collect_local_ci_cleanup_plan",
)


def collect_local_ci_cleanup_plan(
    bindings: Mapping[str, Any],
    queue: list[dict],
    *,
    keep_results: int | None = None,
    keep_logs: int | None = None,
    keep_bundles: int = 0,
    include_prepared: bool = False,
) -> dict:
    retention = cleanup_plan_retention_values(bindings, keep_results=keep_results, keep_logs=keep_logs)
    return _binding(bindings, "_cleanup").collect_local_ci_cleanup_plan(
        queue,
        **retention,
        keep_bundles=keep_bundles,
        include_prepared=include_prepared,
        **cleanup_plan_collect_dependencies(bindings),
    )


def install_cleanup_plan_collect_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLEANUP_PLAN_COLLECT_EXPORTS,
) -> None:
    known_names = set(CLEANUP_PLAN_COLLECT_EXPORTS)
    collect_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), collect_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
