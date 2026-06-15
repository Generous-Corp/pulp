"""Locked queue enqueue helper."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


def enqueue_job_locked(
    branch: str,
    sha: str,
    priority: str,
    targets: list[str],
    mode: str,
    validation: str,
    submission: dict | None = None,
    *,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    reconcile_running_jobs_unlocked_fn: Callable[[list[dict]], tuple[list[dict], bool]],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
    normalize_priority_fn: Callable[[str], str],
    normalize_validation_mode_fn: Callable[[str], str],
    make_fingerprint_fn: Callable[[str, str, list[str], str], str],
    find_active_job_by_fingerprint_unlocked_fn: Callable[[list[dict], str], dict | None],
    bump_pending_job_priority_unlocked_fn: Callable[[dict, str], bool],
    make_job_fn: Callable[..., dict],
    pending_supersedence_candidates_unlocked_fn: Callable[[list[dict], dict], list[tuple[dict, str]]],
    supersede_job_unlocked_fn: Callable[[dict, str, str], None],
    trim_completed_jobs_fn: Callable[[list[dict]], list[dict]],
    normalize_job_fn: Callable[[dict], dict],
) -> tuple[dict, bool]:
    requested_priority = normalize_priority_fn(priority)
    normalized_validation = normalize_validation_mode_fn(validation)

    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        queue, changed = reconcile_running_jobs_unlocked_fn(queue)
        if changed:
            save_queue_unlocked_fn(queue)
        fingerprint = make_fingerprint_fn(branch, sha, targets, normalized_validation)

        existing = find_active_job_by_fingerprint_unlocked_fn(queue, fingerprint)
        if existing is not None:
            if bump_pending_job_priority_unlocked_fn(existing, requested_priority):
                save_queue_unlocked_fn(queue)
            return normalize_job_fn(existing), False

        job = make_job_fn(branch, sha, requested_priority, targets, mode, normalized_validation, submission=submission)
        queue.append(job)
        for existing, reason in pending_supersedence_candidates_unlocked_fn(queue, job):
            supersede_job_unlocked_fn(existing, job["id"], reason)
        save_queue_unlocked_fn(trim_completed_jobs_fn(queue))
        return job, True
