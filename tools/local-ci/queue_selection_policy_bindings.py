"""Facade bindings for queue selection and status grouping helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_SELECTION_POLICY_EXPORTS = (
    "job_sort_key",
    "queue_status_groups",
    "recent_completed_jobs_for_status",
    "find_job_unlocked",
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


def install_queue_selection_policy_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_SELECTION_POLICY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
