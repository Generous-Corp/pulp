"""Queue command lookup and priority mutation helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable

from git_helpers import now_iso
from normalize import normalize_priority
from queue_selection import find_job_unlocked


def find_queue_command_job_unlocked(queue: list[dict], job_ref: str) -> dict | None:
    return find_job_unlocked(queue, job_ref, statuses={"pending", "running"})


def set_pending_job_priority_unlocked(
    job: dict,
    requested_priority: str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    if job.get("status") != "pending":
        return False

    job["priority"] = normalize_priority(requested_priority)
    job["bumped_at"] = now_iso_fn()
    return True
