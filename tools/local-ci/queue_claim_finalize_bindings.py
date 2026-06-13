"""Bindings from the local_ci facade to queue claim/finalize helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding


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
