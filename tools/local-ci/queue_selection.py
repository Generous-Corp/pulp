"""Queue sorting, grouping, claiming, and lookup helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable

from git_helpers import now_iso
from normalize import priority_value


def job_sort_key(job: dict) -> tuple[int, str, str]:
    return (-priority_value(job.get("priority", "normal")), job.get("queued_at", ""), job["id"])


def queue_status_groups(queue: list[dict]) -> tuple[list[dict], list[dict], list[dict]]:
    pending = sorted([job for job in queue if job.get("status") == "pending"], key=job_sort_key)
    running = [job for job in queue if job.get("status") == "running"]
    completed = [job for job in queue if job.get("status") == "completed"]
    return pending, running, completed


def recent_completed_jobs_for_status(completed_jobs: list[dict], *, limit: int = 5) -> list[dict]:
    if limit <= 0:
        return []
    return completed_jobs[-limit:]


def claim_next_job_unlocked(
    queue: list[dict],
    *,
    runner: dict,
    now_iso_fn: Callable[[], str] = now_iso,
) -> dict | None:
    pending = sorted(
        [job for job in queue if job.get("status") == "pending"],
        key=job_sort_key,
    )
    if not pending:
        return None

    selected_id = pending[0]["id"]
    for job in queue:
        if job["id"] != selected_id:
            continue
        job["status"] = "running"
        job["started_at"] = now_iso_fn()
        job["runner"] = runner
        job.pop("active_targets", None)
        job.pop("last_progress_at", None)
        return job

    return None


def find_job_unlocked(queue: list[dict], job_ref: str, statuses: set[str] | None = None) -> dict | None:
    candidates = queue
    if statuses is not None:
        candidates = [job for job in candidates if job.get("status") in statuses]

    for job in candidates:
        if job["id"] == job_ref:
            return job

    id_prefix = [job for job in candidates if job["id"].startswith(job_ref)]
    if len(id_prefix) == 1:
        return id_prefix[0]
    if len(id_prefix) > 1:
        raise ValueError(f"Job reference '{job_ref}' is ambiguous.")

    branch_matches = [job for job in candidates if job.get("branch") == job_ref]
    if len(branch_matches) == 1:
        return branch_matches[0]
    if len(branch_matches) > 1:
        raise ValueError(
            f"Multiple jobs match branch '{job_ref}'. Use a job id or unique prefix."
        )

    return None


def select_job_for_logs(queue: list[dict], runner_info: dict | None, job_ref: str | None) -> dict | None:
    if job_ref:
        return find_job_unlocked(queue, job_ref)

    if runner_info and runner_info.get("active_job_id"):
        return find_job_unlocked(queue, runner_info["active_job_id"])

    for job in reversed(queue):
        if job.get("status") == "completed":
            return job
    return None
