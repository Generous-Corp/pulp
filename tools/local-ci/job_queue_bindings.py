"""Bindings from the local_ci facade to queue persistence helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


JOB_QUEUE_EXPORTS = (
    "normalize_job",
    "load_queue_unlocked",
    "save_queue_unlocked",
)


def normalize_job(bindings: Mapping[str, Any], job: dict) -> dict:
    return _binding(bindings, "_job_queue").normalize_job(job)


def load_queue_unlocked(bindings: Mapping[str, Any]) -> list[dict]:
    return _binding(bindings, "_job_queue").load_queue_unlocked()


def save_queue_unlocked(bindings: Mapping[str, Any], queue: list[dict]) -> None:
    return _binding(bindings, "_job_queue").save_queue_unlocked(queue)


def install_job_queue_helpers(bindings: dict[str, Any], names: tuple[str, ...] = JOB_QUEUE_EXPORTS) -> None:
    known_names = set(JOB_QUEUE_EXPORTS)
    queue_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), queue_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
