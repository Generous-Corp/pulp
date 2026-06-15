"""Queue drain loop and wait helpers for local CI jobs."""

from __future__ import annotations

from collections.abc import Callable
import os
from pathlib import Path


def scheduler_error_result(job: dict, exc: Exception, *, now_fn: Callable[[], str]) -> dict:
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
                    result = scheduler_error_result(job, exc, now_fn=now_fn)

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
