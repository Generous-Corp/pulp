"""Pure queue policy helpers for local CI.

This module owns job identity, enqueue duplicate/priority policy, enqueue
supersedence candidate selection, queue-command lookup and priority mutation,
priority ordering, supersedence, cancellation result payloads, summaries,
target-state status detail formatting, status active-target selection and
recent-completed selection, log-command line fragments,
queue-command result line fragments, queue and result status line fragments,
runner status line fragments, recent-completed result summaries,
stale-running job selection/replacement/requeue state, stale-running
reconciliation action selection, runner-info active-target mutation,
initial/progress/completed target-state payloads, target-state snapshots,
completed-job state mutation, queue status grouping, and completed-queue retention.
Higher-level queue mutation, locking, runner liveness, result persistence, and
drain orchestration live in queue_lifecycle.py and runner_state.py, with
queue_bindings.py preserving the historical local_ci.py facade exports.
"""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

from git_helpers import now_iso
from normalize import normalize_priority, normalize_validation_mode, priority_value
from provenance import normalize_provenance, provenance_summary
from queue_display import (
    empty_log_line,
    job_logs_header_line,
    log_section_header_line,
    missing_job_logs_line,
    missing_log_files_line,
    recent_completed_missing_result_line,
    recent_completed_status_line,
    result_execution_line,
    result_overall_line,
    result_target_lines,
    result_validation_line,
    status_active_targets,
    status_runner_line,
    status_submission_lines,
    status_target_detail_lines,
    status_target_states,
    summarize_active_targets,
    summarize_job,
    target_result_line,
    target_state_detail_parts,
)
from queue_jobs import (
    ROOT,
    bump_pending_job_priority_unlocked,
    default_priority_for,
    find_active_job_by_fingerprint_unlocked,
    make_fingerprint,
    make_job,
    validate_ci_branch_name,
)


def find_queue_command_job_unlocked(queue: list[dict], job_ref: str) -> dict | None:
    return find_job_unlocked(queue, job_ref, statuses={"pending", "running"})


def set_pending_job_priority_unlocked(
    job: dict,
    requested_priority: str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    if job.get("status") != "pending":
        return False

    job["priority"] = normalize_priority(requested_priority)
    job["bumped_at"] = now_iso_fn()
    return True


def supersedence_key(job: dict) -> tuple[str, tuple[str, ...], str]:
    return (
        job.get("branch", ""),
        tuple(sorted(job.get("targets") or [])),
        normalize_validation_mode(job.get("validation", "full")),
    )


def supersedence_identity_key(job: dict) -> tuple[str, str, str]:
    return (
        job.get("branch", ""),
        job.get("sha", ""),
        normalize_validation_mode(job.get("validation", "full")),
    )


def jobs_share_supersedence_scope(newer_job: dict, older_job: dict) -> bool:
    return (
        newer_job.get("id") != older_job.get("id")
        and newer_job.get("fingerprint") != older_job.get("fingerprint")
        and supersedence_key(newer_job) == supersedence_key(older_job)
    )


def job_has_narrower_same_identity_scope(newer_job: dict, older_job: dict) -> bool:
    if (
        newer_job.get("id") == older_job.get("id")
        or newer_job.get("fingerprint") == older_job.get("fingerprint")
        or supersedence_identity_key(newer_job) != supersedence_identity_key(older_job)
    ):
        return False

    newer_targets = set(newer_job.get("targets") or [])
    older_targets = set(older_job.get("targets") or [])
    return bool(newer_targets) and newer_targets < older_targets


def supersedence_reason(newer_job: dict, older_job: dict) -> str | None:
    if jobs_share_supersedence_scope(newer_job, older_job):
        return "newer_sha_queued"
    if job_has_narrower_same_identity_scope(newer_job, older_job):
        return "narrower_scope_queued"
    return None


def pending_supersedence_candidates_unlocked(queue: list[dict], newer_job: dict) -> list[tuple[dict, str]]:
    candidates: list[tuple[dict, str]] = []
    for job in queue:
        if job.get("status") != "pending":
            continue
        reason = supersedence_reason(newer_job, job)
        if reason:
            candidates.append((job, reason))
    return candidates


def supersedence_result(
    job: dict,
    superseded_by: str,
    reason: str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> dict:
    return {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "validation": job.get("validation", "full"),
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": now_iso_fn(),
        "provenance": normalize_provenance(job.get("provenance")),
        "results": [],
        "overall": "superseded",
        "superseded_by": superseded_by,
        "superseded_reason": reason,
    }


def cancellation_result(
    job: dict,
    reason: str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> dict:
    return {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "validation": job.get("validation", "full"),
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": now_iso_fn(),
        "provenance": normalize_provenance(job.get("provenance")),
        "results": [],
        "overall": "canceled",
        "canceled_reason": reason,
    }


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


def enqueue_command_result_line(job: dict, *, created: bool) -> str:
    prefix = "Enqueued" if created else "Already queued/running"
    return f"{prefix}: {summarize_job(job)}"


def drain_runner_active_line(runner_info: dict | None) -> str:
    if runner_info and runner_info.get("active_job_id"):
        return (
            f"Another local CI runner is active [{runner_info['active_job_id']}] "
            f"{runner_info.get('active_branch', '?')}."
        )
    return "Another local CI runner is active."


def complete_job_with_result_unlocked(job: dict, result: dict, result_path: Path | str) -> None:
    job["status"] = "completed"
    job["completed_at"] = result["completed_at"]
    job["result_file"] = str(result_path)
    job["overall"] = result.get("overall")
    for key in ("superseded_by", "superseded_reason", "canceled_reason"):
        if key in result:
            job[key] = result[key]
    job.pop("runner", None)
    job.pop("active_targets", None)
    job.pop("last_progress_at", None)


def update_runner_info_active_targets(
    info: dict,
    job_id: str,
    active_targets: dict | None,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    if info.get("active_job_id") != job_id:
        return False

    if active_targets:
        info["active_targets"] = active_targets
    else:
        info.pop("active_targets", None)
    info["updated_at"] = now_iso_fn()
    return True


def initial_target_state(*, started_at: str, log_path: str) -> dict:
    return {
        "status": "running",
        "started_at": started_at,
        "phase": "starting",
        "log_path": log_path,
    }


def updated_target_state(previous_state: dict | None, fields: dict) -> dict:
    state = dict(previous_state or {})
    for key, value in fields.items():
        if value is None:
            state.pop(key, None)
        else:
            state[key] = value
    return state


def completed_target_state(
    result: dict,
    previous_state: dict | None,
    *,
    completed_at: str,
    default_log_path: str,
) -> dict:
    previous_state = previous_state or {}
    return {
        "status": result.get("status", "?"),
        "exit_code": result.get("exit_code"),
        "duration_secs": result.get("duration_secs"),
        "completed_at": completed_at,
        "phase": "done" if result.get("status") == "pass" else previous_state.get("phase", "done"),
        "log_path": result.get("log_file", default_log_path),
        "last_output_at": previous_state.get("last_output_at"),
        "last_line": previous_state.get("last_line"),
        "host": previous_state.get("host"),
        "transport_mode": result.get("transport_mode", previous_state.get("transport_mode")),
        "wait_reason": previous_state.get("wait_reason"),
    }


def target_state_snapshot(target_states: dict[str, dict]) -> dict | None:
    snapshot = {name: dict(state) for name, state in target_states.items()}
    return snapshot or None


def find_stale_running_replacement_unlocked(queue: list[dict], job: dict) -> tuple[dict | None, str | None]:
    replacement = None
    replacement_reason = None
    for candidate in queue:
        if candidate.get("status") not in {"pending", "running"}:
            continue
        reason = supersedence_reason(candidate, job)
        if not reason:
            continue
        if replacement is None or candidate.get("queued_at", "") > replacement.get("queued_at", ""):
            replacement = candidate
            replacement_reason = reason
    return replacement, replacement_reason


def stale_running_reconciliation_actions_unlocked(queue: list[dict], stale_jobs: list[dict]) -> list[dict]:
    actions: list[dict] = []
    for job in stale_jobs:
        replacement, reason = find_stale_running_replacement_unlocked(queue, job)
        if replacement is not None:
            actions.append(
                {
                    "action": "supersede",
                    "job": job,
                    "replacement": replacement,
                    "reason": reason or "newer_sha_queued",
                }
            )
        else:
            actions.append({"action": "requeue", "job": job})
    return actions


def stale_running_jobs_for_runner_unlocked(queue: list[dict], runner_pid: int | None) -> list[dict]:
    stale: list[dict] = []
    for job in queue:
        if job.get("status") != "running":
            continue
        job_runner = job.get("runner") or {}
        if runner_pid and job_runner.get("pid") == runner_pid:
            continue
        stale.append(job)
    return stale


def requeue_stale_running_job_unlocked(
    job: dict,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> None:
    job["status"] = "pending"
    job["requeued_at"] = now_iso_fn()
    job.pop("started_at", None)
    job.pop("runner", None)


def upsert_job_active_targets_unlocked(
    queue: list[dict],
    job_id: str,
    active_targets: dict | None,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    for job in queue:
        if job["id"] != job_id:
            continue
        if active_targets:
            job["active_targets"] = active_targets
            job["last_progress_at"] = now_iso_fn()
        else:
            job.pop("active_targets", None)
            job.pop("last_progress_at", None)
        return True
    return False


def update_job_target_state_unlocked(
    queue: list[dict],
    job_id: str,
    target_name: str,
    fields: dict,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    job = find_job_unlocked(queue, job_id)
    if job is None:
        return False

    active_targets = dict(job.get("active_targets") or {})
    state = updated_target_state(active_targets.get(target_name), fields)

    if state:
        active_targets[target_name] = state
    else:
        active_targets.pop(target_name, None)

    upsert_job_active_targets_unlocked(
        queue,
        job["id"],
        active_targets if active_targets else None,
        now_iso_fn=now_iso_fn,
    )
    return True


def complete_job_unlocked(
    queue: list[dict],
    job_id: str,
    result: dict,
    result_path: Path | str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    for job in queue:
        if job["id"] != job_id:
            continue
        job["status"] = "completed"
        job["completed_at"] = now_iso_fn()
        job["result_file"] = str(result_path)
        job["overall"] = result.get("overall")
        job.pop("runner", None)
        job.pop("active_targets", None)
        job.pop("last_progress_at", None)
        return True
    return False


def trim_completed_jobs_with_removed_ids(
    queue: list[dict],
    *,
    keep_completed_jobs: int,
) -> tuple[list[dict], set[str]]:
    completed = [job for job in queue if job.get("status") == "completed"]
    if len(completed) <= keep_completed_jobs:
        return queue, set()

    completed_by_time = sorted(completed, key=lambda job: job.get("completed_at", job.get("queued_at", "")))
    remove_ids = {job["id"] for job in completed_by_time[:-keep_completed_jobs]}
    return [job for job in queue if job["id"] not in remove_ids], remove_ids


def trim_completed_jobs(queue: list[dict], *, keep_completed_jobs: int) -> list[dict]:
    trimmed, _removed_ids = trim_completed_jobs_with_removed_ids(
        queue,
        keep_completed_jobs=keep_completed_jobs,
    )
    return trimmed


def job_sort_key(job: dict) -> tuple[int, str, str]:
    return (-priority_value(job.get("priority", "normal")), job.get("queued_at", ""), job["id"])


def queue_status_groups(queue: list[dict]) -> tuple[list[dict], list[dict], list[dict]]:
    pending = sorted([job for job in queue if job.get("status") == "pending"], key=job_sort_key)
    running = [job for job in queue if job.get("status") == "running"]
    completed = [job for job in queue if job.get("status") == "completed"]
    return pending, running, completed


def recent_completed_jobs_for_status(completed_jobs: list[dict], *, limit: int = 5) -> list[dict]:
    if limit <= 0:
        return []
    return completed_jobs[-limit:]


def claim_next_job_unlocked(
    queue: list[dict],
    *,
    runner: dict,
    now_iso_fn: Callable[[], str] = now_iso,
) -> dict | None:
    pending = sorted(
        [job for job in queue if job.get("status") == "pending"],
        key=job_sort_key,
    )
    if not pending:
        return None

    selected_id = pending[0]["id"]
    for job in queue:
        if job["id"] != selected_id:
            continue
        job["status"] = "running"
        job["started_at"] = now_iso_fn()
        job["runner"] = runner
        job.pop("active_targets", None)
        job.pop("last_progress_at", None)
        return job

    return None


def find_job_unlocked(queue: list[dict], job_ref: str, statuses: set[str] | None = None) -> dict | None:
    candidates = queue
    if statuses is not None:
        candidates = [job for job in candidates if job.get("status") in statuses]

    for job in candidates:
        if job["id"] == job_ref:
            return job

    id_prefix = [job for job in candidates if job["id"].startswith(job_ref)]
    if len(id_prefix) == 1:
        return id_prefix[0]
    if len(id_prefix) > 1:
        raise ValueError(f"Job reference '{job_ref}' is ambiguous.")

    branch_matches = [job for job in candidates if job.get("branch") == job_ref]
    if len(branch_matches) == 1:
        return branch_matches[0]
    if len(branch_matches) > 1:
        raise ValueError(
            f"Multiple jobs match branch '{job_ref}'. Use a job id or unique prefix."
        )

    return None


def select_job_for_logs(queue: list[dict], runner_info: dict | None, job_ref: str | None) -> dict | None:
    if job_ref:
        return find_job_unlocked(queue, job_ref)

    if runner_info and runner_info.get("active_job_id"):
        return find_job_unlocked(queue, runner_info["active_job_id"])

    for job in reversed(queue):
        if job.get("status") == "completed":
            return job
    return None
