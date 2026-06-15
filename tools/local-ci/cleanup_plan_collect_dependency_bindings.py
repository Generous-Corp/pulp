"""Dependency assembly for cleanup plan collection bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def cleanup_plan_retention_values(
    bindings: Mapping[str, Any],
    *,
    keep_results: int | None,
    keep_logs: int | None,
) -> dict[str, int]:
    default_keep = _binding(bindings, "KEEP_COMPLETED_JOBS")
    return {
        "keep_results": default_keep if keep_results is None else keep_results,
        "keep_logs": default_keep if keep_logs is None else keep_logs,
    }


def cleanup_plan_collect_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "bundles_dir_fn": _binding(bindings, "bundles_dir"),
        "logs_dir_fn": _binding(bindings, "logs_dir"),
        "results_dir_fn": _binding(bindings, "results_dir"),
        "prepared_dir_fn": _binding(bindings, "prepared_dir"),
        "path_size_bytes_fn": _binding(bindings, "path_size_bytes"),
    }
