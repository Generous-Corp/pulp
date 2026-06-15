"""Cleanup artifact plan collection helpers."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

from cleanup_artifact_identity import artifact_entry_sort_key, result_file_job_id


DEFAULT_KEEP_COMPLETED_JOBS = 25


def collect_local_ci_cleanup_plan(
    queue: list[dict],
    *,
    keep_results: int = DEFAULT_KEEP_COMPLETED_JOBS,
    keep_logs: int = DEFAULT_KEEP_COMPLETED_JOBS,
    keep_bundles: int = 0,
    include_prepared: bool = False,
    bundles_dir_fn: Callable[[], Path],
    logs_dir_fn: Callable[[], Path],
    results_dir_fn: Callable[[], Path],
    prepared_dir_fn: Callable[[], Path],
    path_size_bytes_fn: Callable[[Path], int],
) -> dict:
    keep_results = max(0, int(keep_results))
    keep_logs = max(0, int(keep_logs))
    keep_bundles = max(0, int(keep_bundles))
    retained_job_ids = {job["id"] for job in queue}
    live_job_ids = {job["id"] for job in queue if job.get("status") in {"pending", "running"}}
    categories: dict[str, list[dict]] = {
        "bundles": [],
        "logs": [],
        "results": [],
        "prepared": [],
    }

    def add_file_entry(category: str, path: Path, job_id: str | None) -> None:
        try:
            stat = path.stat()
        except OSError:
            return
        categories[category].append(
            {
                "path": path,
                "job_id": job_id,
                "size_bytes": int(stat.st_size),
                "mtime": float(stat.st_mtime),
            }
        )

    def add_dir_entry(category: str, path: Path, job_id: str | None) -> None:
        if not path.exists() or not path.is_dir():
            return
        try:
            stat = path.stat()
        except OSError:
            return
        categories[category].append(
            {
                "path": path,
                "job_id": job_id,
                "size_bytes": path_size_bytes_fn(path),
                "mtime": float(stat.st_mtime),
            }
        )

    for path in bundles_dir_fn().glob("*.bundle"):
        add_file_entry("bundles", path, path.stem)
    log_root = logs_dir_fn()
    for path in (log_root.iterdir() if log_root.exists() else []):
        if path.is_dir():
            add_dir_entry("logs", path, path.name)
    for path in results_dir_fn().glob("*.json"):
        add_file_entry("results", path, result_file_job_id(path))
    prepared_root = prepared_dir_fn()
    if include_prepared and prepared_root.exists():
        for target_dir in prepared_root.iterdir():
            if not target_dir.is_dir():
                continue
            for mode_dir in target_dir.iterdir():
                if mode_dir.is_dir():
                    add_dir_entry("prepared", mode_dir, None)

    plan_categories: dict[str, list[dict]] = {
        "bundles": [],
        "logs": [],
        "results": [],
        "prepared": [],
    }

    bundle_candidates = [
        entry
        for entry in sorted(categories["bundles"], key=artifact_entry_sort_key, reverse=True)
        if entry.get("job_id") not in live_job_ids
    ]
    plan_categories["bundles"] = bundle_candidates[keep_bundles:]

    def select_queue_orphans(entries: list[dict], keep_count: int) -> list[dict]:
        always_keep = [entry for entry in entries if entry.get("job_id") in retained_job_ids]
        orphaned = [entry for entry in entries if entry.get("job_id") not in retained_job_ids]
        orphaned.sort(key=artifact_entry_sort_key, reverse=True)
        del always_keep
        return orphaned[keep_count:]

    plan_categories["logs"] = select_queue_orphans(categories["logs"], keep_logs)
    plan_categories["results"] = select_queue_orphans(categories["results"], keep_results)
    plan_categories["prepared"] = sorted(
        categories["prepared"],
        key=artifact_entry_sort_key,
        reverse=True,
    )

    total_bytes = sum(
        int(entry.get("size_bytes", 0))
        for entries in plan_categories.values()
        for entry in entries
    )
    total_paths = sum(len(entries) for entries in plan_categories.values())
    return {
        "categories": plan_categories,
        "total_bytes": total_bytes,
        "total_paths": total_paths,
        "keep_results": keep_results,
        "keep_logs": keep_logs,
        "keep_bundles": keep_bundles,
        "include_prepared": include_prepared,
    }
