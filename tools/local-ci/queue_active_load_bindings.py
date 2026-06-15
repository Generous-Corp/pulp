"""Bindings from the local_ci facade to queue active-target and load helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from queue_active_load_dependency_bindings import queue_active_target_dependencies
from queue_active_load_dependency_bindings import queue_load_job_dependencies


QUEUE_ACTIVE_LOAD_EXPORTS = (
    "update_job_active_targets",
    "load_job",
)


def update_job_active_targets(bindings: Mapping[str, Any], job_id: str, active_targets: dict | None) -> None:
    _binding(bindings, "_queue_lifecycle").update_job_active_targets_locked(
        job_id,
        active_targets,
        **queue_active_target_dependencies(bindings),
    )


def load_job(bindings: Mapping[str, Any], job_id: str) -> dict | None:
    return _binding(bindings, "_queue_lifecycle").load_job_locked(
        job_id,
        **queue_load_job_dependencies(bindings),
    )


def install_queue_active_load_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_ACTIVE_LOAD_EXPORTS,
) -> None:
    known_names = set(QUEUE_ACTIVE_LOAD_EXPORTS)
    active_load_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), active_load_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
