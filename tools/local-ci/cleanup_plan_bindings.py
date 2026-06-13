"""Facade bindings for local-CI cleanup plan helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def result_file_job_id(bindings: Mapping[str, Any], path: Any) -> str | None:
    return _binding(bindings, "_cleanup").result_file_job_id(path)


def artifact_entry_sort_key(bindings: Mapping[str, Any], entry: dict) -> tuple[float, str]:
    return _binding(bindings, "_cleanup").artifact_entry_sort_key(entry)


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


def apply_local_ci_cleanup_plan(bindings: Mapping[str, Any], plan: dict) -> dict:
    return _binding(bindings, "_cleanup").apply_local_ci_cleanup_plan(plan)


def cleanup_plan_lines(bindings: Mapping[str, Any], plan: dict, *, dry_run: bool) -> list[str]:
    return _binding(bindings, "_cleanup").cleanup_plan_lines(
        plan,
        dry_run=dry_run,
        format_size_fn=_binding(bindings, "format_size_bytes"),
        describe_path_fn=_binding(bindings, "describe_path_for_cleanup"),
    )
