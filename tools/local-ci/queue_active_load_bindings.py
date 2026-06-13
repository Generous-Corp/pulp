"""Bindings from the local_ci facade to queue active-target and load helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


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
