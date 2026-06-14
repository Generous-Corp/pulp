"""Queue terminal result and completed-job helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

from git_helpers import now_iso
from provenance import normalize_provenance


def supersedence_result(
    job: dict,
    superseded_by: str,
    reason: str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> dict:
    return {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "validation": job.get("validation", "full"),
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": now_iso_fn(),
        "provenance": normalize_provenance(job.get("provenance")),
        "results": [],
        "overall": "superseded",
        "superseded_by": superseded_by,
        "superseded_reason": reason,
    }


def cancellation_result(
    job: dict,
    reason: str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> dict:
    return {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "validation": job.get("validation", "full"),
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": now_iso_fn(),
        "provenance": normalize_provenance(job.get("provenance")),
        "results": [],
        "overall": "canceled",
        "canceled_reason": reason,
    }


def complete_job_with_result_unlocked(job: dict, result: dict, result_path: Path | str) -> None:
    job["status"] = "completed"
    job["completed_at"] = result["completed_at"]
    job["result_file"] = str(result_path)
    job["overall"] = result.get("overall")
    for key in ("superseded_by", "superseded_reason", "canceled_reason"):
        if key in result:
            job[key] = result[key]
    job.pop("runner", None)
    job.pop("active_targets", None)
    job.pop("last_progress_at", None)


def complete_superseded_job_unlocked(
    job: dict,
    superseded_by: str,
    reason: str,
    *,
    supersedence_result_fn: Callable[[dict, str, str], dict],
    save_result_fn: Callable[[dict], Path | str],
    complete_job_with_result_unlocked_fn: Callable[[dict, dict, Path | str], None],
) -> Path | str:
    result = supersedence_result_fn(job, superseded_by, reason)
    result_path = save_result_fn(result)
    complete_job_with_result_unlocked_fn(job, result, result_path)
    return result_path


def complete_canceled_job_unlocked(
    job: dict,
    reason: str,
    *,
    cancellation_result_fn: Callable[[dict, str], dict],
    save_result_fn: Callable[[dict], Path | str],
    complete_job_with_result_unlocked_fn: Callable[[dict, dict, Path | str], None],
) -> Path | str:
    result = cancellation_result_fn(job, reason)
    result_path = save_result_fn(result)
    complete_job_with_result_unlocked_fn(job, result, result_path)
    return result_path


def complete_job_unlocked(
    queue: list[dict],
    job_id: str,
    result: dict,
    result_path: Path | str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    for job in queue:
        if job["id"] != job_id:
            continue
        job["status"] = "completed"
        job["completed_at"] = now_iso_fn()
        job["result_file"] = str(result_path)
        job["overall"] = result.get("overall")
        job.pop("runner", None)
        job.pop("active_targets", None)
        job.pop("last_progress_at", None)
        return True
    return False


def trim_completed_jobs_with_removed_ids(
    queue: list[dict],
    *,
    keep_completed_jobs: int,
) -> tuple[list[dict], set[str]]:
    completed = [job for job in queue if job.get("status") == "completed"]
    if len(completed) <= keep_completed_jobs:
        return queue, set()

    completed_by_time = sorted(completed, key=lambda job: job.get("completed_at", job.get("queued_at", "")))
    remove_ids = {job["id"] for job in completed_by_time[:-keep_completed_jobs]}
    return [job for job in queue if job["id"] not in remove_ids], remove_ids


def trim_completed_jobs(queue: list[dict], *, keep_completed_jobs: int) -> list[dict]:
    trimmed, _removed_ids = trim_completed_jobs_with_removed_ids(
        queue,
        keep_completed_jobs=keep_completed_jobs,
    )
    return trimmed
