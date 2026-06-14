"""Facade bindings for queue job construction helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


QUEUE_JOB_FACTORY_EXPORTS = ("make_job",)


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


def install_queue_job_factory_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_JOB_FACTORY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
