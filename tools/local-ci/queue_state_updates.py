"""Locked active-target and target-state update helpers."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


def update_job_active_targets_locked(
    job_id: str,
    active_targets: dict | None,
    *,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    upsert_job_active_targets_unlocked_fn: Callable[[list[dict], str, dict | None], bool],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
) -> None:
    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        if upsert_job_active_targets_unlocked_fn(queue, job_id, active_targets):
            save_queue_unlocked_fn(queue)


def update_job_target_state_locked(
    job_id: str,
    target_name: str,
    fields: dict,
    *,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    update_job_target_state_unlocked_fn: Callable[[list[dict], str, str, dict], bool],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
) -> None:
    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        if update_job_target_state_unlocked_fn(queue, job_id, target_name, fields):
            save_queue_unlocked_fn(queue)
