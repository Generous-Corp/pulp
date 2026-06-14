"""Facade bindings for queue job policy helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


QUEUE_JOB_POLICY_EXPORTS = (
    "default_priority_for",
    "make_fingerprint",
    "make_job",
    "validate_ci_branch_name",
)


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


def validate_ci_branch_name(bindings: Mapping[str, Any], branch: str) -> str:
    return _binding(bindings, "_queue_orchestrator").validate_ci_branch_name(branch)


def install_queue_job_policy_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_JOB_POLICY_EXPORTS,
) -> None:
    known_names = set(QUEUE_JOB_POLICY_EXPORTS)
    job_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), job_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
