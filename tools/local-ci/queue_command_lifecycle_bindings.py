"""Bindings from the local_ci facade to locked queue command lifecycle helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def supersede_job_unlocked(bindings: Mapping[str, Any], job: dict, superseded_by: str, reason: str) -> None:
    _binding(bindings, "_queue_lifecycle").complete_superseded_job_unlocked(
        job,
        superseded_by,
        reason,
        supersedence_result_fn=_binding(bindings, "supersedence_result"),
        save_result_fn=_binding(bindings, "save_result"),
        complete_job_with_result_unlocked_fn=_binding(
            bindings,
            "_queue_orchestrator",
        ).complete_job_with_result_unlocked,
    )


def cancel_job_unlocked(bindings: Mapping[str, Any], job: dict, reason: str = "operator_canceled") -> None:
    _binding(bindings, "_queue_lifecycle").complete_canceled_job_unlocked(
        job,
        reason,
        cancellation_result_fn=_binding(bindings, "cancellation_result"),
        save_result_fn=_binding(bindings, "save_result"),
        complete_job_with_result_unlocked_fn=_binding(
            bindings,
            "_queue_orchestrator",
        ).complete_job_with_result_unlocked,
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
