"""Bindings from the local_ci facade to locked queue lifecycle helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


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


def load_queue(bindings: Mapping[str, Any]) -> list[dict]:
    with _binding(bindings, "file_lock")(_binding(bindings, "queue_lock_path")(), blocking=True):
        queue = _binding(bindings, "load_queue_unlocked")()
        queue, changed = _binding(bindings, "reconcile_running_jobs_unlocked")(queue)
        if changed:
            _binding(bindings, "save_queue_unlocked")(queue)
        return queue


def update_job_active_targets(bindings: Mapping[str, Any], job_id: str, active_targets: dict | None) -> None:
    _binding(bindings, "_queue_lifecycle").update_job_active_targets_locked(
        job_id,
        active_targets,
        queue_lock_path_fn=_binding(bindings, "queue_lock_path"),
        file_lock_fn=_binding(bindings, "file_lock"),
        load_queue_unlocked_fn=_binding(bindings, "load_queue_unlocked"),
        upsert_job_active_targets_unlocked_fn=_binding(bindings, "upsert_job_active_targets_unlocked"),
        save_queue_unlocked_fn=_binding(bindings, "save_queue_unlocked"),
    )


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
    queue_orchestrator = _binding(bindings, "_queue_orchestrator")

    return _binding(bindings, "_queue_lifecycle").enqueue_job_locked(
        branch,
        sha,
        priority,
        targets,
        mode,
        validation,
        submission=submission,
        queue_lock_path_fn=_binding(bindings, "queue_lock_path"),
        file_lock_fn=_binding(bindings, "file_lock"),
        load_queue_unlocked_fn=_binding(bindings, "load_queue_unlocked"),
        reconcile_running_jobs_unlocked_fn=_binding(bindings, "reconcile_running_jobs_unlocked"),
        save_queue_unlocked_fn=_binding(bindings, "save_queue_unlocked"),
        normalize_priority_fn=_binding(bindings, "normalize_priority"),
        normalize_validation_mode_fn=_binding(bindings, "normalize_validation_mode"),
        make_fingerprint_fn=_binding(bindings, "make_fingerprint"),
        find_active_job_by_fingerprint_unlocked_fn=queue_orchestrator.find_active_job_by_fingerprint_unlocked,
        bump_pending_job_priority_unlocked_fn=lambda existing, requested_priority: queue_orchestrator.bump_pending_job_priority_unlocked(
            existing,
            requested_priority,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
        make_job_fn=_binding(bindings, "make_job"),
        pending_supersedence_candidates_unlocked_fn=queue_orchestrator.pending_supersedence_candidates_unlocked,
        supersede_job_unlocked_fn=_binding(bindings, "supersede_job_unlocked"),
        trim_completed_jobs_fn=_binding(bindings, "trim_completed_jobs"),
        normalize_job_fn=_binding(bindings, "normalize_job"),
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


def load_job(bindings: Mapping[str, Any], job_id: str) -> dict | None:
    return _binding(bindings, "_queue_lifecycle").load_job_locked(
        job_id,
        queue_lock_path_fn=_binding(bindings, "queue_lock_path"),
        file_lock_fn=_binding(bindings, "file_lock"),
        load_queue_unlocked_fn=_binding(bindings, "load_queue_unlocked"),
        reconcile_running_jobs_unlocked_fn=_binding(bindings, "reconcile_running_jobs_unlocked"),
        save_queue_unlocked_fn=_binding(bindings, "save_queue_unlocked"),
        find_job_unlocked_fn=_binding(bindings, "find_job_unlocked"),
        normalize_job_fn=_binding(bindings, "normalize_job"),
    )


def claim_next_job(bindings: Mapping[str, Any]) -> dict | None:
    queue_orchestrator = _binding(bindings, "_queue_orchestrator")

    return _binding(bindings, "_queue_lifecycle").claim_next_job_locked(
        root=_binding(bindings, "ROOT"),
        queue_lock_path_fn=_binding(bindings, "queue_lock_path"),
        file_lock_fn=_binding(bindings, "file_lock"),
        load_queue_unlocked_fn=_binding(bindings, "load_queue_unlocked"),
        reconcile_running_jobs_unlocked_fn=_binding(bindings, "reconcile_running_jobs_unlocked"),
        save_queue_unlocked_fn=_binding(bindings, "save_queue_unlocked"),
        claim_next_job_unlocked_fn=lambda queue, *, runner: queue_orchestrator.claim_next_job_unlocked(
            queue,
            runner=runner,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
        normalize_job_fn=_binding(bindings, "normalize_job"),
        pid_fn=_binding(bindings, "os").getpid,
    )


def finalize_job(bindings: Mapping[str, Any], job_id: str, result: dict, result_path: Path) -> None:
    queue_orchestrator = _binding(bindings, "_queue_orchestrator")

    _binding(bindings, "_queue_lifecycle").finalize_job_locked(
        job_id,
        result,
        result_path,
        queue_lock_path_fn=_binding(bindings, "queue_lock_path"),
        file_lock_fn=_binding(bindings, "file_lock"),
        load_queue_unlocked_fn=_binding(bindings, "load_queue_unlocked"),
        complete_job_unlocked_fn=lambda queue, current_job_id, current_result, current_result_path: queue_orchestrator.complete_job_unlocked(
            queue,
            current_job_id,
            current_result,
            current_result_path,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
        trim_completed_jobs_with_removed_ids_fn=_binding(bindings, "trim_completed_jobs_with_removed_ids"),
        save_queue_unlocked_fn=_binding(bindings, "save_queue_unlocked"),
        collect_local_ci_cleanup_plan_fn=_binding(bindings, "collect_local_ci_cleanup_plan"),
        apply_local_ci_cleanup_plan_fn=_binding(bindings, "apply_local_ci_cleanup_plan"),
        keep_results=_binding(bindings, "KEEP_COMPLETED_JOBS"),
        keep_logs=_binding(bindings, "KEEP_COMPLETED_JOBS"),
        keep_bundles=0,
        include_prepared=False,
    )


def wait_for_job(bindings: Mapping[str, Any], job_id: str, config: dict) -> tuple[dict | None, int]:
    return _binding(bindings, "_queue_lifecycle").wait_for_job_completion(
        job_id,
        config,
        load_job_fn=_binding(bindings, "load_job"),
        load_result_fn=_binding(bindings, "load_result"),
        drain_pending_jobs_fn=_binding(bindings, "drain_pending_jobs"),
        current_runner_info_fn=_binding(bindings, "current_runner_info"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
        poll_secs=_binding(bindings, "WAIT_POLL_SECS"),
    )


def drain_pending_jobs(bindings: Mapping[str, Any], config: dict, *, blocking: bool) -> tuple[bool, bool]:
    return _binding(bindings, "_queue_lifecycle").drain_pending_jobs_locked(
        config,
        blocking=blocking,
        root=_binding(bindings, "ROOT"),
        drain_lock_path_fn=_binding(bindings, "drain_lock_path"),
        file_lock_fn=_binding(bindings, "file_lock"),
        lock_busy_error_cls=_binding(bindings, "LockBusyError"),
        write_runner_info_fn=_binding(bindings, "write_runner_info"),
        clear_runner_info_fn=_binding(bindings, "clear_runner_info"),
        reclaim_stale_remote_validators_fn=_binding(bindings, "reclaim_stale_remote_validators"),
        claim_next_job_fn=_binding(bindings, "claim_next_job"),
        process_job_fn=_binding(bindings, "process_job"),
        save_result_fn=_binding(bindings, "save_result"),
        finalize_job_fn=_binding(bindings, "finalize_job"),
        print_result_fn=_binding(bindings, "print_result"),
        now_fn=_binding(bindings, "now_iso"),
        pid_fn=_binding(bindings, "os").getpid,
    )
