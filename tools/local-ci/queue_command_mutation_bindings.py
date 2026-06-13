"""Bindings from the local_ci facade to queue command mutation helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_COMMAND_MUTATION_EXPORTS = (
    "bump_queue_command_job",
    "cancel_queue_command_job",
)


def bump_queue_command_job(bindings: Mapping[str, Any], job_ref: str, requested_priority: str) -> dict:
    queue_orchestrator = _binding(bindings, "_queue_orchestrator")

    return _binding(bindings, "_queue_lifecycle").bump_queue_command_job_locked(
        job_ref,
        requested_priority,
        queue_lock_path_fn=_binding(bindings, "queue_lock_path"),
        file_lock_fn=_binding(bindings, "file_lock"),
        load_queue_unlocked_fn=_binding(bindings, "load_queue_unlocked"),
        find_queue_command_job_unlocked_fn=queue_orchestrator.find_queue_command_job_unlocked,
        set_pending_job_priority_unlocked_fn=lambda job, priority: queue_orchestrator.set_pending_job_priority_unlocked(
            job,
            priority,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
        save_queue_unlocked_fn=_binding(bindings, "save_queue_unlocked"),
        summarize_job_fn=_binding(bindings, "summarize_job"),
    )


def cancel_queue_command_job(bindings: Mapping[str, Any], job_ref: str) -> dict:
    return _binding(bindings, "_queue_lifecycle").cancel_queue_command_job_locked(
        job_ref,
        queue_lock_path_fn=_binding(bindings, "queue_lock_path"),
        file_lock_fn=_binding(bindings, "file_lock"),
        load_queue_unlocked_fn=_binding(bindings, "load_queue_unlocked"),
        find_queue_command_job_unlocked_fn=_binding(bindings, "_queue_orchestrator").find_queue_command_job_unlocked,
        cancel_job_unlocked_fn=_binding(bindings, "cancel_job_unlocked"),
        trim_completed_jobs_fn=_binding(bindings, "trim_completed_jobs"),
        save_queue_unlocked_fn=_binding(bindings, "save_queue_unlocked"),
        summarize_job_fn=_binding(bindings, "summarize_job"),
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
