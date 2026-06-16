"""Bindings from the local_ci facade to queue command mutation helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from queue_command_mutation_dependency_bindings import queue_bump_command_dependencies
from queue_command_mutation_dependency_bindings import queue_cancel_command_dependencies


QUEUE_COMMAND_MUTATION_EXPORTS = (
    "bump_queue_command_job",
    "cancel_queue_command_job",
)


def bump_queue_command_job(bindings: Mapping[str, Any], job_ref: str, requested_priority: str) -> dict:
    return _binding(bindings, "_queue_lifecycle").bump_queue_command_job_locked(
        job_ref,
        requested_priority,
        **queue_bump_command_dependencies(bindings),
    )


def cancel_queue_command_job(bindings: Mapping[str, Any], job_ref: str) -> dict:
    return _binding(bindings, "_queue_lifecycle").cancel_queue_command_job_locked(
        job_ref,
        **queue_cancel_command_dependencies(bindings),
    )


def install_queue_command_mutation_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_COMMAND_MUTATION_EXPORTS,
) -> None:
    known_names = set(QUEUE_COMMAND_MUTATION_EXPORTS)
    mutation_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), mutation_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
