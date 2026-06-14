"""Dependency assembly for locked queue enqueue bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def queue_enqueue_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    queue_orchestrator = _binding(bindings, "_queue_orchestrator")
    return {
        "queue_lock_path_fn": _binding(bindings, "queue_lock_path"),
        "file_lock_fn": _binding(bindings, "file_lock"),
        "load_queue_unlocked_fn": _binding(bindings, "load_queue_unlocked"),
        "reconcile_running_jobs_unlocked_fn": _binding(bindings, "reconcile_running_jobs_unlocked"),
        "save_queue_unlocked_fn": _binding(bindings, "save_queue_unlocked"),
        "normalize_priority_fn": _binding(bindings, "normalize_priority"),
        "normalize_validation_mode_fn": _binding(bindings, "normalize_validation_mode"),
        "make_fingerprint_fn": _binding(bindings, "make_fingerprint"),
        "find_active_job_by_fingerprint_unlocked_fn": queue_orchestrator.find_active_job_by_fingerprint_unlocked,
        "bump_pending_job_priority_unlocked_fn": lambda existing, requested_priority: queue_orchestrator.bump_pending_job_priority_unlocked(
            existing,
            requested_priority,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
        "make_job_fn": _binding(bindings, "make_job"),
        "pending_supersedence_candidates_unlocked_fn": queue_orchestrator.pending_supersedence_candidates_unlocked,
        "supersede_job_unlocked_fn": _binding(bindings, "supersede_job_unlocked"),
        "trim_completed_jobs_fn": _binding(bindings, "trim_completed_jobs"),
        "normalize_job_fn": _binding(bindings, "normalize_job"),
    }
