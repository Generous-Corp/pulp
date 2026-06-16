"""Facade bindings for queue completed-job retention helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_RETENTION_TRIM_EXPORTS = (
    "trim_completed_jobs_with_removed_ids",
    "trim_completed_jobs",
)


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


def install_queue_retention_trim_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_RETENTION_TRIM_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
