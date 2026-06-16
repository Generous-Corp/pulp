"""Locked queue job finalization helper."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


def finalize_job_locked(
    job_id: str,
    result: dict,
    result_path: Path,
    *,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    complete_job_unlocked_fn: Callable[..., object],
    trim_completed_jobs_with_removed_ids_fn: Callable[[list[dict]], tuple[list[dict], set[str]]],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
    collect_local_ci_cleanup_plan_fn: Callable[..., dict],
    apply_local_ci_cleanup_plan_fn: Callable[[dict], dict],
    keep_results: int,
    keep_logs: int,
    keep_bundles: int = 0,
    include_prepared: bool = False,
) -> None:
    retained_queue: list[dict] | None = None
    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        complete_job_unlocked_fn(queue, job_id, result, result_path)
        retained_queue, _removed_ids = trim_completed_jobs_with_removed_ids_fn(queue)
        save_queue_unlocked_fn(retained_queue)

    if retained_queue is not None:
        apply_local_ci_cleanup_plan_fn(
            collect_local_ci_cleanup_plan_fn(
                retained_queue,
                keep_results=keep_results,
                keep_logs=keep_logs,
                keep_bundles=keep_bundles,
                include_prepared=include_prepared,
            )
        )
