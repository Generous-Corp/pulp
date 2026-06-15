"""Compatibility facade for validation logging and progress helpers."""

from __future__ import annotations

from pathlib import Path

from validation_logged_command import run_logged_command as _run_logged_command
from validation_progress_marker import parse_progress_marker


HEARTBEAT_INTERVAL_SECS = 15.0
STUCK_IDLE_SECS = 90.0


def run_logged_command(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    input_text: str | None = None,
    timeout: int = 3600,
    log_path: Path | None = None,
    report_progress=None,
    heartbeat_interval_secs: float = HEARTBEAT_INTERVAL_SECS,
    stuck_idle_secs: float = STUCK_IDLE_SECS,
) -> dict:
    return _run_logged_command(
        cmd,
        cwd=cwd,
        input_text=input_text,
        timeout=timeout,
        log_path=log_path,
        report_progress=report_progress,
        heartbeat_interval_secs=heartbeat_interval_secs,
        stuck_idle_secs=stuck_idle_secs,
    )


__all__ = (
    "HEARTBEAT_INTERVAL_SECS",
    "STUCK_IDLE_SECS",
    "parse_progress_marker",
    "run_logged_command",
)
