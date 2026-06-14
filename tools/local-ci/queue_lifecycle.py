"""Locked queue lifecycle helpers for local CI jobs."""

from __future__ import annotations

from collections.abc import Callable
import os
from pathlib import Path

from queue_command_lifecycle import bump_queue_command_job_locked, cancel_queue_command_job_locked
from queue_completion import (
    complete_canceled_job_unlocked,
    complete_superseded_job_unlocked,
)
from queue_runner_lifecycle import (
    drain_pending_jobs_locked,
    scheduler_error_result,
    wait_for_job_completion,
)
from queue_state_updates import (
    update_job_active_targets_locked,
    update_job_target_state_locked,
)
from queue_stale_reclaim_lifecycle import reclaim_stale_remote_validators_locked


def reconcile_running_jobs_unlocked(
    queue: list[dict],
    *,
    stale_running_jobs_unlocked_fn: Callable[[list[dict]], list[dict]],
    stale_running_reconciliation_actions_unlocked_fn: Callable[[list[dict], list[dict]], list[dict]],
    supersede_job_unlocked_fn: Callable[[dict, str, str], None],
    requeue_stale_running_job_unlocked_fn: Callable[[dict], None],
) -> tuple[list[dict], bool]:
    changed = False
    stale_jobs = list(stale_running_jobs_unlocked_fn(queue))
    while True:
        current_stale_jobs = [job for job in stale_jobs if job.get("status") == "running"]
        if not current_stale_jobs:
            break
        actions = stale_running_reconciliation_actions_unlocked_fn(queue, current_stale_jobs)
        if not actions:
            break
        action_applied = False
        for action in actions:
            job = action["job"]
            if job.get("status") != "running":
                continue
            if action["action"] == "supersede":
                supersede_job_unlocked_fn(job, action["replacement"]["id"], action["reason"])
                changed = True
                action_applied = True
                break

            requeue_stale_running_job_unlocked_fn(job)
            changed = True
            action_applied = True
            break
        if not action_applied:
            break

    return queue, changed


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


def claim_next_job_locked(
    *,
    root: Path | str,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    reconcile_running_jobs_unlocked_fn: Callable[[list[dict]], tuple[list[dict], bool]],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
    claim_next_job_unlocked_fn: Callable[..., dict | None],
    normalize_job_fn: Callable[[dict], dict],
    pid_fn: Callable[[], int] = os.getpid,
) -> dict | None:
    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        queue, changed = reconcile_running_jobs_unlocked_fn(queue)
        if changed:
            save_queue_unlocked_fn(queue)
        claimed = claim_next_job_unlocked_fn(
            queue,
            runner={"pid": pid_fn(), "root": str(root)},
        )
        if claimed is None:
            return None

        save_queue_unlocked_fn(queue)
        return normalize_job_fn(claimed)


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
