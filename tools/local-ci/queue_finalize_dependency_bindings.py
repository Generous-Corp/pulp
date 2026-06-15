"""Dependency assembly for queue finalize facade bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def queue_finalize_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    queue_orchestrator = _binding(bindings, "_queue_orchestrator")

    return {
        "queue_lock_path_fn": _binding(bindings, "queue_lock_path"),
        "file_lock_fn": _binding(bindings, "file_lock"),
        "load_queue_unlocked_fn": _binding(bindings, "load_queue_unlocked"),
        "complete_job_unlocked_fn": lambda queue, current_job_id, current_result, current_result_path: queue_orchestrator.complete_job_unlocked(
            queue,
            current_job_id,
            current_result,
            current_result_path,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
        "trim_completed_jobs_with_removed_ids_fn": _binding(bindings, "trim_completed_jobs_with_removed_ids"),
        "save_queue_unlocked_fn": _binding(bindings, "save_queue_unlocked"),
        "collect_local_ci_cleanup_plan_fn": _binding(bindings, "collect_local_ci_cleanup_plan"),
        "apply_local_ci_cleanup_plan_fn": _binding(bindings, "apply_local_ci_cleanup_plan"),
        "keep_results": _binding(bindings, "KEEP_COMPLETED_JOBS"),
        "keep_logs": _binding(bindings, "KEEP_COMPLETED_JOBS"),
        "keep_bundles": 0,
        "include_prepared": False,
    }
