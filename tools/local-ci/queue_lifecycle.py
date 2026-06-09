"""Locked queue lifecycle helpers for local CI jobs."""

from __future__ import annotations

from collections.abc import Callable
import os
from pathlib import Path


def load_job_locked(
    job_id: str,
    *,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    reconcile_running_jobs_unlocked_fn: Callable[[list[dict]], tuple[list[dict], bool]],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
    find_job_unlocked_fn: Callable[[list[dict], str], dict | None],
    normalize_job_fn: Callable[[dict], dict],
) -> dict | None:
    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        queue, changed = reconcile_running_jobs_unlocked_fn(queue)
        if changed:
            save_queue_unlocked_fn(queue)
        job = find_job_unlocked_fn(queue, job_id)
        return normalize_job_fn(job) if job else None


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


def finalize_job_locked(
    job_id: str,
    result: dict,
    result_path: Path,
    *,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    complete_job_unlocked_fn: Callable[..., object],
    trim_completed_jobs_with_removed_ids_fn: Callable[[list[dict]], tuple[list[dict], set[str]]],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
    collect_local_ci_cleanup_plan_fn: Callable[..., dict],
    apply_local_ci_cleanup_plan_fn: Callable[[dict], dict],
    keep_results: int,
    keep_logs: int,
    keep_bundles: int = 0,
    include_prepared: bool = False,
) -> None:
    retained_queue: list[dict] | None = None
    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        complete_job_unlocked_fn(queue, job_id, result, result_path)
        retained_queue, _removed_ids = trim_completed_jobs_with_removed_ids_fn(queue)
        save_queue_unlocked_fn(retained_queue)

    if retained_queue is not None:
        apply_local_ci_cleanup_plan_fn(
            collect_local_ci_cleanup_plan_fn(
                retained_queue,
                keep_results=keep_results,
                keep_logs=keep_logs,
                keep_bundles=keep_bundles,
                include_prepared=include_prepared,
            )
        )


def wait_for_job_completion(
    job_id: str,
    config: dict,
    *,
    load_job_fn: Callable[[str], dict | None],
    load_result_fn: Callable[[Path], dict],
    drain_pending_jobs_fn: Callable[..., tuple[bool, bool]],
    current_runner_info_fn: Callable[[], dict | None],
    sleep_fn: Callable[[float], None],
    poll_secs: float,
    print_fn: Callable[[str], None] = print,
) -> tuple[dict | None, int]:
    announced_wait = False

    while True:
        job = load_job_fn(job_id)
        if job is None:
            print_fn(f"Job not found: {job_id}")
            return None, 1

        if job.get("status") == "completed":
            result_file = job.get("result_file")
            if not result_file:
                print_fn(f"Job completed without a result file: {job_id}")
                return None, 1
            result = load_result_fn(Path(result_file))
            return result, 0 if result.get("overall") == "pass" else 1

        acquired, _ = drain_pending_jobs_fn(config, blocking=False)
        if acquired:
            continue

        runner = current_runner_info_fn()
        if runner and not announced_wait:
            active_job = runner.get("active_job_id")
            active_branch = runner.get("active_branch")
            if active_job and active_branch:
                print_fn(
                    f"Another local CI runner is active [{active_job}] {active_branch}; waiting for {job_id}..."
                )
            else:
                print_fn("Another local CI runner is active; waiting for queued job completion...")
            announced_wait = True

        sleep_fn(poll_secs)
