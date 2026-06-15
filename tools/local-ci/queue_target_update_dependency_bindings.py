"""Dependency assembly for queue target-state update facade bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def queue_target_update_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "queue_lock_path_fn": _binding(bindings, "queue_lock_path"),
        "file_lock_fn": _binding(bindings, "file_lock"),
        "load_queue_unlocked_fn": _binding(bindings, "load_queue_unlocked"),
        "update_job_target_state_unlocked_fn": lambda queue, current_job_id, current_target_name, current_fields: _binding(
            bindings,
            "_queue_orchestrator",
        ).update_job_target_state_unlocked(
            queue,
            current_job_id,
            current_target_name,
            current_fields,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
        "save_queue_unlocked_fn": _binding(bindings, "save_queue_unlocked"),
    }
