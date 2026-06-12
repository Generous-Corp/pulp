"""Bindings from the local_ci facade to locked queue lifecycle helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from queue_command_lifecycle_bindings import (
    bump_queue_command_job,
    cancel_job_unlocked,
    cancel_queue_command_job,
    supersede_job_unlocked,
)
from queue_drain_bindings import (
    claim_next_job,
    drain_pending_jobs,
    finalize_job,
    wait_for_job,
)
from queue_state_lifecycle_bindings import (
    load_job,
    reclaim_stale_remote_validators,
    reconcile_running_jobs_unlocked,
    update_job_active_targets,
    update_job_target_state,
)


def load_queue(bindings: Mapping[str, Any]) -> list[dict]:
    with _binding(bindings, "file_lock")(_binding(bindings, "queue_lock_path")(), blocking=True):
        queue = _binding(bindings, "load_queue_unlocked")()
        queue, changed = _binding(bindings, "reconcile_running_jobs_unlocked")(queue)
        if changed:
            _binding(bindings, "save_queue_unlocked")(queue)
        return queue


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
