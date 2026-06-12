"""Bindings from the local_ci facade to queue policy helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def default_priority_for(bindings: Mapping[str, Any], command: str, config: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").default_priority_for(command, config)


def make_fingerprint(bindings: Mapping[str, Any], branch: str, sha: str, targets: list[str], validation: str) -> str:
    return _binding(bindings, "_queue_orchestrator").make_fingerprint(branch, sha, targets, validation)


def make_job(
    bindings: Mapping[str, Any],
    branch: str,
    sha: str,
    priority: str,
    targets: list[str],
    mode: str,
    validation: str,
    submission: dict | None = None,
) -> dict:
    return _binding(bindings, "_queue_orchestrator").make_job(
        branch,
        sha,
        priority,
        targets,
        mode,
        validation,
        submission=submission,
        now_iso_fn=_binding(bindings, "now_iso"),
        uuid_hex_fn=lambda: _binding_attr(bindings, "uuid", "uuid4")().hex,
        root=_binding(bindings, "ROOT"),
        validate_branch_fn=_binding(bindings, "validate_ci_branch_name"),
    )


def supersedence_result(bindings: Mapping[str, Any], job: dict, superseded_by: str, reason: str) -> dict:
    return _binding(bindings, "_queue_orchestrator").supersedence_result(
        job,
        superseded_by,
        reason,
        now_iso_fn=_binding(bindings, "now_iso"),
    )


def cancellation_result(bindings: Mapping[str, Any], job: dict, reason: str) -> dict:
    return _binding(bindings, "_queue_orchestrator").cancellation_result(
        job,
        reason,
        now_iso_fn=_binding(bindings, "now_iso"),
    )


def supersedence_key(bindings: Mapping[str, Any], job: dict) -> tuple[str, tuple[str, ...], str]:
    return _binding(bindings, "_queue_orchestrator").supersedence_key(job)


def supersedence_identity_key(bindings: Mapping[str, Any], job: dict) -> tuple[str, str, str]:
    return _binding(bindings, "_queue_orchestrator").supersedence_identity_key(job)


def jobs_share_supersedence_scope(bindings: Mapping[str, Any], newer_job: dict, older_job: dict) -> bool:
    return _binding(bindings, "_queue_orchestrator").jobs_share_supersedence_scope(newer_job, older_job)


def job_has_narrower_same_identity_scope(bindings: Mapping[str, Any], newer_job: dict, older_job: dict) -> bool:
    return _binding(bindings, "_queue_orchestrator").job_has_narrower_same_identity_scope(newer_job, older_job)


def supersedence_reason(bindings: Mapping[str, Any], newer_job: dict, older_job: dict) -> str | None:
    return _binding(bindings, "_queue_orchestrator").supersedence_reason(newer_job, older_job)


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


def validate_ci_branch_name(bindings: Mapping[str, Any], branch: str) -> str:
    return _binding(bindings, "_queue_orchestrator").validate_ci_branch_name(branch)
