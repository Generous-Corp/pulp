"""Bindings from the local_ci facade to stale remote validator reclaim."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


QUEUE_STALE_RECLAIM_EXPORTS = ("reclaim_stale_remote_validators",)


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
