"""Locked queue stale-running reconciliation helpers."""

from __future__ import annotations

from collections.abc import Callable


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
