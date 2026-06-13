"""Facade bindings for queue supersedence policy helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


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
