"""Stale Windows validator cleanup reclaim dispatch."""

from __future__ import annotations

from collections.abc import Callable

from cleanup_stale_windows_result import stale_windows_validator_update_fields


def reclaim_stale_remote_validator_candidates(
    candidates: list[dict],
    *,
    cleanup_validator_fn: Callable[[str, int, str], dict],
    update_job_target_state_fn: Callable,
    now_fn: Callable[[], str],
    trim_line_fn: Callable[[str], str],
) -> int:
    for candidate in candidates:
        result = cleanup_validator_fn(
            candidate["host"],
            candidate["validator_pid"],
            candidate["validator_started_at"],
        )
        update_job_target_state_fn(
            candidate["job_id"],
            candidate["target"],
            **stale_windows_validator_update_fields(
                candidate,
                result,
                now_fn=now_fn,
                trim_line_fn=trim_line_fn,
            ),
        )
    return len(candidates)


__all__ = ["reclaim_stale_remote_validator_candidates"]
