"""Dependency assembly for queue active-target and load facade bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def queue_active_target_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "queue_lock_path_fn": _binding(bindings, "queue_lock_path"),
        "file_lock_fn": _binding(bindings, "file_lock"),
        "load_queue_unlocked_fn": _binding(bindings, "load_queue_unlocked"),
        "upsert_job_active_targets_unlocked_fn": _binding(bindings, "upsert_job_active_targets_unlocked"),
        "save_queue_unlocked_fn": _binding(bindings, "save_queue_unlocked"),
    }


def queue_load_job_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "queue_lock_path_fn": _binding(bindings, "queue_lock_path"),
        "file_lock_fn": _binding(bindings, "file_lock"),
        "load_queue_unlocked_fn": _binding(bindings, "load_queue_unlocked"),
        "reconcile_running_jobs_unlocked_fn": _binding(bindings, "reconcile_running_jobs_unlocked"),
        "save_queue_unlocked_fn": _binding(bindings, "save_queue_unlocked"),
        "find_job_unlocked_fn": _binding(bindings, "find_job_unlocked"),
        "normalize_job_fn": _binding(bindings, "normalize_job"),
    }
