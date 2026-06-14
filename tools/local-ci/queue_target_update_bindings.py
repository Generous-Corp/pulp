"""Bindings from the local_ci facade to locked queue target-state updates."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


QUEUE_TARGET_UPDATE_EXPORTS = ("update_job_target_state",)


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
