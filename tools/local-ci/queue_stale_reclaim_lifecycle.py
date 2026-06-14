"""Locked stale remote-validator reclaim orchestration."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


def reclaim_stale_remote_validators_locked(
    *,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    collect_stale_windows_cleanup_candidates_unlocked_fn: Callable[[list[dict]], list[dict]],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
    reclaim_stale_remote_validator_candidates_fn: Callable[..., int],
    cleanup_validator_fn: Callable[[str, int, str], dict],
    update_job_target_state_fn: Callable[..., None],
    now_fn: Callable[[], str],
    trim_line_fn: Callable[[str], str],
) -> int:
    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        candidates = collect_stale_windows_cleanup_candidates_unlocked_fn(queue)
        if candidates:
            save_queue_unlocked_fn(queue)

    return reclaim_stale_remote_validator_candidates_fn(
        candidates,
        cleanup_validator_fn=cleanup_validator_fn,
        update_job_target_state_fn=update_job_target_state_fn,
        now_fn=now_fn,
        trim_line_fn=trim_line_fn,
    )
