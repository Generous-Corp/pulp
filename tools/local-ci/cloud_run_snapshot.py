"""Pure GitHub Actions run snapshot helpers for cloud records."""
from __future__ import annotations

from cloud_records import (
    duration_between,
    normalize_cloud_record,
    normalize_github_timestamp,
)


def summarize_cloud_timing(snapshot: dict) -> dict[str, str | float | None]:
    created_at = normalize_github_timestamp(snapshot.get("createdAt"))
    updated_at = normalize_github_timestamp(snapshot.get("updatedAt"))
    observed_updates = [updated_at] if updated_at else []
    job_starts = [
        normalize_github_timestamp(job.get("startedAt"))
        for job in snapshot.get("jobs", []) or []
        if normalize_github_timestamp(job.get("startedAt"))
    ]
    job_completions = [
        normalize_github_timestamp(job.get("completedAt"))
        for job in snapshot.get("jobs", []) or []
        if normalize_github_timestamp(job.get("completedAt"))
    ]
    for job in snapshot.get("jobs", []) or []:
        for step in job.get("steps", []) or []:
            step_started = normalize_github_timestamp(step.get("startedAt"))
            if step_started:
                observed_updates.append(step_started)
            step_completed = normalize_github_timestamp(step.get("completedAt"))
            if step_completed:
                observed_updates.append(step_completed)

    started_at = min(job_starts) if job_starts else ""
    status = snapshot.get("status", "")
    if status == "completed":
        if job_completions:
            completed_at = max(job_completions)
        else:
            completed_at = updated_at
    else:
        completed_at = ""

    duration_anchor = completed_at or (max(observed_updates) if observed_updates else "")
    return {
        "started_at": started_at,
        "completed_at": completed_at,
        "queue_delay_secs": duration_between(created_at, started_at),
        "duration_secs": duration_between(started_at, duration_anchor),
    }


def snapshot_jobs(snapshot: dict) -> list[dict]:
    jobs = []
    for job in snapshot.get("jobs", []) or []:
        jobs.append(
            {
                "name": job.get("name", ""),
                "status": job.get("status", ""),
                "conclusion": job.get("conclusion", ""),
                "started_at": normalize_github_timestamp(job.get("startedAt", "")),
                "completed_at": normalize_github_timestamp(job.get("completedAt", "")),
            }
        )
    return jobs


def update_cloud_record_from_run(
    record: dict,
    snapshot: dict,
    *,
    provider_resolved: str | None = None,
    now_iso_fn,
) -> dict:
    updated = normalize_cloud_record(record)
    snapshot_updated_at = snapshot.get("updatedAt") or now_iso_fn()
    current_updated_at = updated.get("updated_at") or ""
    if current_updated_at and snapshot_updated_at and current_updated_at > snapshot_updated_at:
        return updated

    updated["run_id"] = snapshot.get("databaseId", updated.get("run_id"))
    updated["workflow_name"] = snapshot.get("workflowName", updated.get("workflow_name"))
    updated["head_branch"] = snapshot.get("headBranch", updated.get("head_branch"))
    updated["head_sha"] = snapshot.get("headSha", updated.get("head_sha"))
    updated["status"] = snapshot.get("status", updated.get("status"))
    updated["conclusion"] = snapshot.get("conclusion") or updated.get("conclusion", "")
    updated["url"] = snapshot.get("url", updated.get("url"))
    updated["updated_at"] = snapshot_updated_at
    if provider_resolved:
        updated["provider_resolved"] = provider_resolved
    if snapshot.get("createdAt") and not updated.get("matched_at"):
        updated["matched_at"] = snapshot["createdAt"]

    timing = summarize_cloud_timing(snapshot)
    if timing.get("started_at"):
        updated["started_at"] = timing["started_at"]
    if timing.get("completed_at"):
        updated["completed_at"] = timing["completed_at"]
    elif updated.get("status") == "completed" and not updated.get("completed_at"):
        updated["completed_at"] = snapshot_updated_at
    elif updated.get("status") != "completed":
        updated["completed_at"] = ""
    updated["queue_delay_secs"] = timing.get("queue_delay_secs")
    updated["duration_secs"] = timing.get("duration_secs")

    jobs = snapshot_jobs(snapshot)
    if jobs:
        updated["jobs"] = jobs
    return updated
