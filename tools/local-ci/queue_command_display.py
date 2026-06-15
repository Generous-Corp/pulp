"""Queue command line formatting helpers."""

from __future__ import annotations

from git_helpers import short_sha


def summarize_job(job: dict) -> str:
    targets = ",".join(job.get("targets") or []) or "none"
    validation = job.get("validation", "full")
    validation_suffix = f" validation={validation}" if validation != "full" else ""
    return (
        f"[{job['id']}] {job['branch']} @ {short_sha(job.get('sha', ''))} "
        f"priority={job.get('priority', 'normal')} targets={targets}{validation_suffix}"
    )


def enqueue_command_result_line(job: dict, *, created: bool) -> str:
    prefix = "Enqueued" if created else "Already queued/running"
    return f"{prefix}: {summarize_job(job)}"


def bump_queue_command_result_line(result: dict, job_ref: str) -> tuple[int, str]:
    if result["status"] == "missing":
        return 1, f"No active job matches '{job_ref}'."
    if result["status"] == "not_pending":
        return 1, f"Job is already {result['job_status']}; only pending jobs can be reprioritized."
    return 0, f"Updated priority: {result['summary']}"


def cancel_queue_command_result_line(result: dict, job_ref: str) -> tuple[int, str]:
    if result["status"] == "missing":
        return 1, f"No active job matches '{job_ref}'."
    if result["status"] == "not_pending":
        return 1, f"Job is already {result['job_status']}; only pending jobs can be canceled safely."
    return 0, f"Canceled: {result['summary']}"


def drain_runner_active_line(runner_info: dict | None) -> str:
    if runner_info and runner_info.get("active_job_id"):
        return (
            f"Another local CI runner is active [{runner_info['active_job_id']}] "
            f"{runner_info.get('active_branch', '?')}."
        )
    return "Another local CI runner is active."
