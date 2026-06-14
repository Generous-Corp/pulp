"""Bindings from the local_ci facade to locked queue enqueue helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from queue_enqueue_dependency_bindings import queue_enqueue_dependencies


QUEUE_ENQUEUE_EXPORTS = ("enqueue_job",)


def enqueue_job(
    bindings: Mapping[str, Any],
    branch: str,
    sha: str,
    priority: str,
    targets: list[str],
    mode: str,
    validation: str,
    submission: dict | None = None,
) -> tuple[dict, bool]:
    return _binding(bindings, "_queue_lifecycle").enqueue_job_locked(
        branch,
        sha,
        priority,
        targets,
        mode,
        validation,
        submission=submission,
        **queue_enqueue_dependencies(bindings),
    )


def install_queue_enqueue_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_ENQUEUE_EXPORTS,
) -> None:
    known_names = set(QUEUE_ENQUEUE_EXPORTS)
    enqueue_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), enqueue_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
