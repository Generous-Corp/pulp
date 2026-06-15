"""Bindings from the local_ci facade to queue wait/drain helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from queue_wait_drain_dependency_bindings import queue_drain_dependencies
from queue_wait_drain_dependency_bindings import queue_wait_dependencies


QUEUE_WAIT_DRAIN_EXPORTS = (
    "wait_for_job",
    "drain_pending_jobs",
)


def wait_for_job(bindings: Mapping[str, Any], job_id: str, config: dict) -> tuple[dict | None, int]:
    return _binding(bindings, "_queue_lifecycle").wait_for_job_completion(
        job_id,
        config,
        **queue_wait_dependencies(bindings),
    )


def drain_pending_jobs(bindings: Mapping[str, Any], config: dict, *, blocking: bool) -> tuple[bool, bool]:
    return _binding(bindings, "_queue_lifecycle").drain_pending_jobs_locked(
        config,
        blocking=blocking,
        **queue_drain_dependencies(bindings),
    )


def install_queue_wait_drain_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_WAIT_DRAIN_EXPORTS,
) -> None:
    known_names = set(QUEUE_WAIT_DRAIN_EXPORTS)
    wait_drain_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), wait_drain_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
