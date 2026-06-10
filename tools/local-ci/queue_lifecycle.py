"""Locked queue lifecycle helpers for local CI jobs."""

from __future__ import annotations

from collections.abc import Callable
import os
from pathlib import Path


def reconcile_running_jobs_unlocked(
    queue: list[dict],
    *,
    stale_running_jobs_unlocked_fn: Callable[[list[dict]], list[dict]],
    stale_running_reconciliation_actions_unlocked_fn: Callable[[list[dict], list[dict]], list[dict]],
    supersede_job_unlocked_fn: Callable[[dict, str, str], None],
    requeue_stale_running_job_unlocked_fn: Callable[[dict], None],
) -> tuple[list[dict], bool]:
    changed = False
    actions = stale_running_reconciliation_actions_unlocked_fn(
        queue,
        stale_running_jobs_unlocked_fn(queue),
    )
    for action in actions:
        job = action["job"]
        if action["action"] == "supersede":
            supersede_job_unlocked_fn(job, action["replacement"]["id"], action["reason"])
            changed = True
            continue

        requeue_stale_running_job_unlocked_fn(job)
        changed = True

    return queue, changed


def enqueue_job_locked(
    branch: str,
    sha: str,
    priority: str,
    targets: list[str],
    mode: str,
    validation: str,
    submission: dict | None = None,
    *,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    reconcile_running_jobs_unlocked_fn: Callable[[list[dict]], tuple[list[dict], bool]],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
    normalize_priority_fn: Callable[[str], str],
    normalize_validation_mode_fn: Callable[[str], str],
    make_fingerprint_fn: Callable[[str, str, list[str], str], str],
    find_active_job_by_fingerprint_unlocked_fn: Callable[[list[dict], str], dict | None],
    bump_pending_job_priority_unlocked_fn: Callable[[dict, str], bool],
    make_job_fn: Callable[..., dict],
    pending_supersedence_candidates_unlocked_fn: Callable[[list[dict], dict], list[tuple[dict, str]]],
    supersede_job_unlocked_fn: Callable[[dict, str, str], None],
    trim_completed_jobs_fn: Callable[[list[dict]], list[dict]],
    normalize_job_fn: Callable[[dict], dict],
) -> tuple[dict, bool]:
    requested_priority = normalize_priority_fn(priority)
    normalized_validation = normalize_validation_mode_fn(validation)

    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        queue, changed = reconcile_running_jobs_unlocked_fn(queue)
        if changed:
            save_queue_unlocked_fn(queue)
        fingerprint = make_fingerprint_fn(branch, sha, targets, normalized_validation)

        existing = find_active_job_by_fingerprint_unlocked_fn(queue, fingerprint)
        if existing is not None:
            if bump_pending_job_priority_unlocked_fn(existing, requested_priority):
                save_queue_unlocked_fn(queue)
            return normalize_job_fn(existing), False

        job = make_job_fn(branch, sha, requested_priority, targets, mode, normalized_validation, submission=submission)
        queue.append(job)
        for existing, reason in pending_supersedence_candidates_unlocked_fn(queue, job):
            supersede_job_unlocked_fn(existing, job["id"], reason)
        save_queue_unlocked_fn(trim_completed_jobs_fn(queue))
        return job, True


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


def bump_queue_command_job_locked(
    job_ref: str,
    requested_priority: str,
    *,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    find_queue_command_job_unlocked_fn: Callable[[list[dict], str], dict | None],
    set_pending_job_priority_unlocked_fn: Callable[[dict, str], bool],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
    summarize_job_fn: Callable[[dict], str],
) -> dict:
    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        job = find_queue_command_job_unlocked_fn(queue, job_ref)
        if job is None:
            return {"status": "missing", "job_ref": job_ref}

        if not set_pending_job_priority_unlocked_fn(job, requested_priority):
            return {"status": "not_pending", "job_status": job["status"]}

        save_queue_unlocked_fn(queue)
        return {"status": "updated", "summary": summarize_job_fn(job)}


def cancel_queue_command_job_locked(
    job_ref: str,
    *,
    queue_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    load_queue_unlocked_fn: Callable[[], list[dict]],
    find_queue_command_job_unlocked_fn: Callable[[list[dict], str], dict | None],
    cancel_job_unlocked_fn: Callable[[dict], None],
    trim_completed_jobs_fn: Callable[[list[dict]], list[dict]],
    save_queue_unlocked_fn: Callable[[list[dict]], None],
    summarize_job_fn: Callable[[dict], str],
) -> dict:
    with file_lock_fn(queue_lock_path_fn(), blocking=True):
        queue = load_queue_unlocked_fn()
        job = find_queue_command_job_unlocked_fn(queue, job_ref)
        if job is None:
            return {"status": "missing", "job_ref": job_ref}

        if job["status"] != "pending":
            return {"status": "not_pending", "job_status": job["status"]}

        cancel_job_unlocked_fn(job)
        save_queue_unlocked_fn(trim_completed_jobs_fn(queue))
        return {"status": "canceled", "summary": summarize_job_fn(job)}


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


def _scheduler_error_result(job: dict, exc: Exception, *, now_fn: Callable[[], str]) -> dict:
    return {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "validation": job.get("validation", "full"),
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": now_fn(),
        "results": [
            {
                "target": "scheduler",
                "status": "error",
                "exit_code": -1,
                "duration_secs": 0,
                "stdout_tail": "",
                "stderr_tail": str(exc),
            }
        ],
        "overall": "fail",
    }


def drain_pending_jobs_locked(
    config: dict,
    *,
    blocking: bool,
    root: Path | str,
    drain_lock_path_fn: Callable[[], Path],
    file_lock_fn,
    lock_busy_error_cls: type[Exception],
    write_runner_info_fn: Callable[[dict], None],
    clear_runner_info_fn: Callable[[], None],
    reclaim_stale_remote_validators_fn: Callable[[dict], int],
    claim_next_job_fn: Callable[[], dict | None],
    process_job_fn: Callable[[dict, dict], dict],
    save_result_fn: Callable[[dict], Path],
    finalize_job_fn: Callable[[str, dict, Path], None],
    print_result_fn: Callable[[dict, Path | None], None],
    now_fn: Callable[[], str],
    pid_fn: Callable[[], int] = os.getpid,
) -> tuple[bool, bool]:
    acquired = False
    try:
        with file_lock_fn(drain_lock_path_fn(), blocking=blocking):
            acquired = True
            runner_info = {
                "pid": pid_fn(),
                "root": str(root),
                "started_at": now_fn(),
                "active_job_id": None,
                "active_branch": None,
            }
            write_runner_info_fn(runner_info)
            any_failure = False

            while True:
                reclaim_stale_remote_validators_fn(config)
                job = claim_next_job_fn()
                if job is None:
                    break

                runner_info.update(
                    {
                        "active_job_id": job["id"],
                        "active_branch": job["branch"],
                        "updated_at": now_fn(),
                    }
                )
                write_runner_info_fn(runner_info)

                try:
                    result = process_job_fn(job, config)
                except Exception as exc:
                    result = _scheduler_error_result(job, exc, now_fn=now_fn)

                result_path = save_result_fn(result)
                finalize_job_fn(job["id"], result, result_path)
                print_result_fn(result, result_path)
                if result["overall"] != "pass":
                    any_failure = True

            return True, any_failure
    except lock_busy_error_cls:
        return False, False
    finally:
        if acquired:
            clear_runner_info_fn()


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
