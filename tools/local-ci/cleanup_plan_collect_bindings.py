"""Facade bindings for cleanup plan collection helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


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
    if keep_results is None:
        keep_results = _binding(bindings, "KEEP_COMPLETED_JOBS")
    if keep_logs is None:
        keep_logs = _binding(bindings, "KEEP_COMPLETED_JOBS")
    return _binding(bindings, "_cleanup").collect_local_ci_cleanup_plan(
        queue,
        keep_results=keep_results,
        keep_logs=keep_logs,
        keep_bundles=keep_bundles,
        include_prepared=include_prepared,
        bundles_dir_fn=_binding(bindings, "bundles_dir"),
        logs_dir_fn=_binding(bindings, "logs_dir"),
        results_dir_fn=_binding(bindings, "results_dir"),
        prepared_dir_fn=_binding(bindings, "prepared_dir"),
        path_size_bytes_fn=_binding(bindings, "path_size_bytes"),
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
