"""Facade bindings for stale Windows validator cleanup helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def collect_stale_windows_cleanup_candidates_unlocked(
    bindings: Mapping[str, Any],
    queue: list[dict],
) -> list[dict]:
    return _binding(bindings, "_cleanup").collect_stale_windows_cleanup_candidates_unlocked(
        queue,
        stale_running_jobs_fn=_binding(bindings, "stale_running_jobs_unlocked"),
        now_fn=_binding(bindings, "now_iso"),
    )


def cleanup_stale_windows_validator(
    bindings: Mapping[str, Any],
    host: str,
    pid: int,
    started_at: str,
) -> dict:
    return _binding(bindings, "_cleanup").cleanup_stale_windows_validator(
        host,
        pid,
        started_at,
        ps_literal_fn=_binding(bindings, "ps_literal"),
        run_logged_command_fn=_binding(bindings, "run_logged_command"),
        windows_ssh_powershell_command_fn=_binding(bindings, "windows_ssh_powershell_command"),
        trim_line_fn=_binding(bindings, "trim_line"),
    )
