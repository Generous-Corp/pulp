"""Locked queue job claim helper."""

from __future__ import annotations

from collections.abc import Callable
import os
from pathlib import Path


def claim_next_job_locked(
    *,
    root: Path | str,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    reconcile_running_jobs_unlocked_fn: Callable[[list[dict]], tuple[list[dict], bool]],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
    claim_next_job_unlocked_fn: Callable[..., dict | None],
    normalize_job_fn: Callable[[dict], dict],
    pid_fn: Callable[[], int] = os.getpid,
) -> dict | None:
    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        queue, changed = reconcile_running_jobs_unlocked_fn(queue)
        if changed:
            save_queue_unlocked_fn(queue)
        claimed = claim_next_job_unlocked_fn(
            queue,
            runner={"pid": pid_fn(), "root": str(root)},
        )
        if claimed is None:
            return None

        save_queue_unlocked_fn(queue)
        return normalize_job_fn(claimed)
