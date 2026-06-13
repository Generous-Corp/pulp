"""Facade bindings for queue retention and selection policy helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def trim_completed_jobs_with_removed_ids(bindings: Mapping[str, Any], queue: list[dict]) -> tuple[list[dict], set[str]]:
    return _binding(bindings, "_queue_orchestrator").trim_completed_jobs_with_removed_ids(
        queue,
        keep_completed_jobs=_binding(bindings, "KEEP_COMPLETED_JOBS"),
    )


def trim_completed_jobs(bindings: Mapping[str, Any], queue: list[dict]) -> list[dict]:
    return _binding(bindings, "_queue_orchestrator").trim_completed_jobs(
        queue,
        keep_completed_jobs=_binding(bindings, "KEEP_COMPLETED_JOBS"),
    )


def job_sort_key(bindings: Mapping[str, Any], job: dict) -> tuple[int, str, str]:
    return _binding(bindings, "_queue_orchestrator").job_sort_key(job)


def queue_status_groups(bindings: Mapping[str, Any], queue: list[dict]) -> tuple[list[dict], list[dict], list[dict]]:
    return _binding(bindings, "_queue_orchestrator").queue_status_groups(queue)


def recent_completed_jobs_for_status(
    bindings: Mapping[str, Any],
    completed_jobs: list[dict],
    *,
    limit: int = 5,
) -> list[dict]:
    return _binding(bindings, "_queue_orchestrator").recent_completed_jobs_for_status(completed_jobs, limit=limit)


def find_job_unlocked(bindings: Mapping[str, Any], queue: list[dict], job_ref: str, statuses: set[str] | None = None) -> dict | None:
    return _binding(bindings, "_queue_orchestrator").find_job_unlocked(queue, job_ref, statuses)
