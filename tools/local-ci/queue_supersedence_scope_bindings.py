"""Facade bindings for queue supersedence key and scope helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_SUPERSEDENCE_SCOPE_EXPORTS = (
    "supersedence_key",
    "supersedence_identity_key",
    "jobs_share_supersedence_scope",
    "job_has_narrower_same_identity_scope",
    "supersedence_reason",
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


def install_queue_supersedence_scope_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_SUPERSEDENCE_SCOPE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
