"""Facade bindings for queue supersedence policy helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_SUPERSEDENCE_POLICY_EXPORTS = (
    "supersedence_result",
    "cancellation_result",
    "supersedence_key",
    "supersedence_identity_key",
    "jobs_share_supersedence_scope",
    "job_has_narrower_same_identity_scope",
    "supersedence_reason",
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


def install_queue_supersedence_policy_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_SUPERSEDENCE_POLICY_EXPORTS,
) -> None:
    known_names = set(QUEUE_SUPERSEDENCE_POLICY_EXPORTS)
    supersedence_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), supersedence_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
