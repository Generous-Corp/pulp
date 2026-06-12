"""Queue target-state payload helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable

from git_helpers import now_iso


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
    job = _find_job_unlocked(queue, job_id)
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


def _find_job_unlocked(queue: list[dict], job_ref: str) -> dict | None:
    for job in queue:
        if job["id"] == job_ref:
            return job

    id_prefix = [job for job in queue if job["id"].startswith(job_ref)]
    if len(id_prefix) == 1:
        return id_prefix[0]
    if len(id_prefix) > 1:
        raise ValueError(f"Job reference '{job_ref}' is ambiguous.")

    branch_matches = [job for job in queue if job.get("branch") == job_ref]
    if len(branch_matches) == 1:
        return branch_matches[0]
    if len(branch_matches) > 1:
        raise ValueError(
            f"Multiple jobs match branch '{job_ref}'. Use a job id or unique prefix."
        )

    return None
