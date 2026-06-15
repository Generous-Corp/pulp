"""Stale Windows validator cleanup candidate selection."""

from __future__ import annotations

from collections.abc import Callable


def collect_stale_windows_cleanup_candidates_unlocked(
    queue: list[dict],
    *,
    stale_running_jobs_fn: Callable[[list[dict]], list[dict]],
    now_fn: Callable[[], str],
) -> list[dict]:
    candidates: list[dict] = []
    for job in stale_running_jobs_fn(queue):
        active_targets = job.get("active_targets") or {}
        state = dict(active_targets.get("windows") or {})
        host = state.get("host")
        validator_pid = state.get("validator_pid")
        validator_started_at = state.get("validator_started_at")
        if not host or validator_pid is None or not validator_started_at:
            continue
        if state.get("cleanup_requested_at"):
            continue

        state["cleanup_requested_at"] = now_fn()
        state["cleanup_status"] = "requested"
        state["cleanup_reason"] = "stale_runner_recovery"
        active_targets["windows"] = state
        job["active_targets"] = active_targets
        job["last_progress_at"] = now_fn()
        candidates.append(
            {
                "job_id": job["id"],
                "target": "windows",
                "host": host,
                "validator_pid": int(validator_pid),
                "validator_started_at": validator_started_at,
            }
        )
    return candidates


__all__ = ["collect_stale_windows_cleanup_candidates_unlocked"]
