"""Bindings from the local_ci facade to queue stale/target-state helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def reconcile_running_jobs_unlocked(bindings: Mapping[str, Any], queue: list[dict]) -> tuple[list[dict], bool]:
    queue_orchestrator = _binding(bindings, "_queue_orchestrator")

    return _binding(bindings, "_queue_lifecycle").reconcile_running_jobs_unlocked(
        queue,
        stale_running_jobs_unlocked_fn=_binding(bindings, "stale_running_jobs_unlocked"),
        stale_running_reconciliation_actions_unlocked_fn=queue_orchestrator.stale_running_reconciliation_actions_unlocked,
        supersede_job_unlocked_fn=_binding(bindings, "supersede_job_unlocked"),
        requeue_stale_running_job_unlocked_fn=lambda job: queue_orchestrator.requeue_stale_running_job_unlocked(
            job,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
    )


def update_job_target_state(bindings: Mapping[str, Any], job_id: str, target_name: str, **fields) -> None:
    _binding(bindings, "_queue_lifecycle").update_job_target_state_locked(
        job_id,
        target_name,
        fields,
        queue_lock_path_fn=_binding(bindings, "queue_lock_path"),
        file_lock_fn=_binding(bindings, "file_lock"),
        load_queue_unlocked_fn=_binding(bindings, "load_queue_unlocked"),
        update_job_target_state_unlocked_fn=lambda queue, current_job_id, current_target_name, current_fields: _binding(
            bindings,
            "_queue_orchestrator",
        ).update_job_target_state_unlocked(
            queue,
            current_job_id,
            current_target_name,
            current_fields,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
        save_queue_unlocked_fn=_binding(bindings, "save_queue_unlocked"),
    )


def reclaim_stale_remote_validators(bindings: Mapping[str, Any], config: dict) -> int:
    return _binding(bindings, "_queue_lifecycle").reclaim_stale_remote_validators_locked(
        queue_lock_path_fn=_binding(bindings, "queue_lock_path"),
        file_lock_fn=_binding(bindings, "file_lock"),
        load_queue_unlocked_fn=_binding(bindings, "load_queue_unlocked"),
        collect_stale_windows_cleanup_candidates_unlocked_fn=_binding(
            bindings,
            "collect_stale_windows_cleanup_candidates_unlocked",
        ),
        save_queue_unlocked_fn=_binding(bindings, "save_queue_unlocked"),
        reclaim_stale_remote_validator_candidates_fn=_binding(bindings, "_cleanup").reclaim_stale_remote_validator_candidates,
        cleanup_validator_fn=_binding(bindings, "cleanup_stale_windows_validator"),
        update_job_target_state_fn=_binding(bindings, "update_job_target_state"),
        now_fn=_binding(bindings, "now_iso"),
        trim_line_fn=_binding(bindings, "trim_line"),
    )
