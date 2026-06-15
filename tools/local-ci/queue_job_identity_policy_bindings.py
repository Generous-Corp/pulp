"""Facade bindings for queue job identity and priority policy helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_JOB_IDENTITY_POLICY_EXPORTS = (
    "default_priority_for",
    "make_fingerprint",
    "validate_ci_branch_name",
)


def default_priority_for(bindings: Mapping[str, Any], command: str, config: dict) -> str:
    return _binding(bindings, "_queue_orchestrator").default_priority_for(command, config)


def make_fingerprint(bindings: Mapping[str, Any], branch: str, sha: str, targets: list[str], validation: str) -> str:
    return _binding(bindings, "_queue_orchestrator").make_fingerprint(branch, sha, targets, validation)


def validate_ci_branch_name(bindings: Mapping[str, Any], branch: str) -> str:
    return _binding(bindings, "_queue_orchestrator").validate_ci_branch_name(branch)


def install_queue_job_identity_policy_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_JOB_IDENTITY_POLICY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
