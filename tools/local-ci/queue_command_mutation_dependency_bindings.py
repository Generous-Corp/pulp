"""Dependency assembly for queue command mutation bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def queue_bump_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    queue_orchestrator = _binding(bindings, "_queue_orchestrator")

    return {
        "queue_lock_path_fn": _binding(bindings, "queue_lock_path"),
        "file_lock_fn": _binding(bindings, "file_lock"),
        "load_queue_unlocked_fn": _binding(bindings, "load_queue_unlocked"),
        "find_queue_command_job_unlocked_fn": queue_orchestrator.find_queue_command_job_unlocked,
        "set_pending_job_priority_unlocked_fn": lambda job, priority: queue_orchestrator.set_pending_job_priority_unlocked(
            job,
            priority,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
        "save_queue_unlocked_fn": _binding(bindings, "save_queue_unlocked"),
        "summarize_job_fn": _binding(bindings, "summarize_job"),
    }


def queue_cancel_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "queue_lock_path_fn": _binding(bindings, "queue_lock_path"),
        "file_lock_fn": _binding(bindings, "file_lock"),
        "load_queue_unlocked_fn": _binding(bindings, "load_queue_unlocked"),
        "find_queue_command_job_unlocked_fn": _binding(bindings, "_queue_orchestrator").find_queue_command_job_unlocked,
        "cancel_job_unlocked_fn": _binding(bindings, "cancel_job_unlocked"),
        "trim_completed_jobs_fn": _binding(bindings, "trim_completed_jobs"),
        "save_queue_unlocked_fn": _binding(bindings, "save_queue_unlocked"),
        "summarize_job_fn": _binding(bindings, "summarize_job"),
    }
