"""Stale-running queue reconciliation helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable

from git_helpers import now_iso
from queue_supersedence import supersedence_reason


def find_stale_running_replacement_unlocked(queue: list[dict], job: dict) -> tuple[dict | None, str | None]:
    replacement = None
    replacement_reason = None
    for candidate in queue:
        if candidate.get("status") not in {"pending", "running"}:
            continue
        reason = supersedence_reason(candidate, job)
        if not reason:
            continue
        if replacement is None or candidate.get("queued_at", "") > replacement.get("queued_at", ""):
            replacement = candidate
            replacement_reason = reason
    return replacement, replacement_reason


def stale_running_reconciliation_actions_unlocked(queue: list[dict], stale_jobs: list[dict]) -> list[dict]:
    actions: list[dict] = []
    for job in stale_jobs:
        replacement, reason = find_stale_running_replacement_unlocked(queue, job)
        if replacement is not None:
            actions.append(
                {
                    "action": "supersede",
                    "job": job,
                    "replacement": replacement,
                    "reason": reason or "newer_sha_queued",
                }
            )
        else:
            actions.append({"action": "requeue", "job": job})
    return actions


def stale_running_jobs_for_runner_unlocked(queue: list[dict], runner_pid: int | None) -> list[dict]:
    stale: list[dict] = []
    for job in queue:
        if job.get("status") != "running":
            continue
        job_runner = job.get("runner") or {}
        if runner_pid and job_runner.get("pid") == runner_pid:
            continue
        stale.append(job)
    return stale


def requeue_stale_running_job_unlocked(
    job: dict,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> None:
    job["status"] = "pending"
    job["requeued_at"] = now_iso_fn()
    job.pop("started_at", None)
    job.pop("runner", None)
