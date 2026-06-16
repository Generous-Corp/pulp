"""Locked queue command mutation helpers for local CI jobs."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


def bump_queue_command_job_locked(
    job_ref: str,
    requested_priority: str,
    *,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    find_queue_command_job_unlocked_fn: Callable[[list[dict], str], dict | None],
    set_pending_job_priority_unlocked_fn: Callable[[dict, str], bool],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
    summarize_job_fn: Callable[[dict], str],
) -> dict:
    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        job = find_queue_command_job_unlocked_fn(queue, job_ref)
        if job is None:
            return {"status": "missing", "job_ref": job_ref}

        if not set_pending_job_priority_unlocked_fn(job, requested_priority):
            return {"status": "not_pending", "job_status": job["status"]}

        save_queue_unlocked_fn(queue)
        return {"status": "updated", "summary": summarize_job_fn(job)}


def cancel_queue_command_job_locked(
    job_ref: str,
    *,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    find_queue_command_job_unlocked_fn: Callable[[list[dict], str], dict | None],
    cancel_job_unlocked_fn: Callable[[dict], None],
    trim_completed_jobs_fn: Callable[[list[dict]], list[dict]],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
    summarize_job_fn: Callable[[dict], str],
) -> dict:
    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        job = find_queue_command_job_unlocked_fn(queue, job_ref)
        if job is None:
            return {"status": "missing", "job_ref": job_ref}

        if job["status"] != "pending":
            return {"status": "not_pending", "job_status": job["status"]}

        cancel_job_unlocked_fn(job)
        save_queue_unlocked_fn(trim_completed_jobs_fn(queue))
        return {"status": "canceled", "summary": summarize_job_fn(job)}
