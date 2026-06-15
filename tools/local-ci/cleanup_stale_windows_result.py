"""Stale Windows validator cleanup result/status helpers."""

from __future__ import annotations

import json
from collections.abc import Callable


def stale_windows_validator_cleanup_status(result: dict) -> str:
    if result.get("killed"):
        return "killed"
    if not result.get("found", True):
        return "not-found"
    if result.get("found") and not result.get("matched", True):
        return "mismatch"
    if result.get("error"):
        return "error"
    return "checked"


def stale_windows_validator_update_fields(
    candidate: dict,
    result: dict,
    *,
    now_fn: Callable[[], str],
    trim_line_fn: Callable[[str], str],
) -> dict:
    clear_process = bool(result.get("killed") or not result.get("found", True))
    return {
        "cleanup_completed_at": now_fn(),
        "cleanup_status": stale_windows_validator_cleanup_status(result),
        "cleanup_result": trim_line_fn(json.dumps(result, sort_keys=True)),
        "validator_pid": None if clear_process else candidate["validator_pid"],
        "validator_started_at": None if clear_process else candidate["validator_started_at"],
    }


__all__ = [
    "stale_windows_validator_cleanup_status",
    "stale_windows_validator_update_fields",
]
