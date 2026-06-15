"""Locked single-job queue load helper."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


def load_job_locked(
    job_id: str,
    *,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    reconcile_running_jobs_unlocked_fn: Callable[[list[dict]], tuple[list[dict], bool]],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
    find_job_unlocked_fn: Callable[[list[dict], str], dict | None],
    normalize_job_fn: Callable[[dict], dict],
) -> dict | None:
    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        queue, changed = reconcile_running_jobs_unlocked_fn(queue)
        if changed:
            save_queue_unlocked_fn(queue)
        job = find_job_unlocked_fn(queue, job_id)
        return normalize_job_fn(job) if job else None
