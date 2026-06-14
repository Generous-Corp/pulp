"""Bindings from the local_ci facade to active-target queue mutation helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from queue_active_target_dependency_bindings import queue_active_target_dependencies


QUEUE_ACTIVE_TARGET_EXPORTS = ("upsert_job_active_targets_unlocked",)


def upsert_job_active_targets_unlocked(
    bindings: Mapping[str, Any],
    queue: list[dict],
    job_id: str,
    active_targets: dict | None,
) -> bool:
    return _binding(bindings, "_queue_orchestrator").upsert_job_active_targets_unlocked(
        queue,
        job_id,
        active_targets,
        **queue_active_target_dependencies(bindings),
    )


def install_queue_active_target_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_ACTIVE_TARGET_EXPORTS,
) -> None:
    known_names = set(QUEUE_ACTIVE_TARGET_EXPORTS)
    active_target_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), active_target_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
