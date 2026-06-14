"""Stale Windows validator cleanup helpers."""

from __future__ import annotations

import json
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


def stale_windows_validator_cleanup_script(
    pid: int,
    started_at: str,
    *,
    ps_literal_fn: Callable[[str], str],
) -> str:
    return f"""
$PidToKill = {pid}
$ExpectedStart = '{ps_literal_fn(started_at)}'

function Get-DescendantProcessIds {{
    param([int]$RootPid)
    $result = New-Object System.Collections.Generic.List[int]
    $queue = New-Object System.Collections.Generic.Queue[int]
    $queue.Enqueue($RootPid)
    while ($queue.Count -gt 0) {{
        $current = $queue.Dequeue()
        $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $current" -ErrorAction SilentlyContinue)
        foreach ($child in $children) {{
            $childPid = [int]$child.ProcessId
            $result.Add($childPid)
            $queue.Enqueue($childPid)
        }}
    }}
    return $result
}}

$result = [ordered]@{{
    found = $false
    matched = $false
    killed = $false
    pid = $PidToKill
}}

try {{
    $proc = Get-Process -Id $PidToKill -ErrorAction SilentlyContinue
    if ($null -ne $proc) {{
        $result.found = $true
        $start = $proc.StartTime.ToUniversalTime().ToString('o')
        $result.start = $start
        if ($ExpectedStart -and $start -ne $ExpectedStart) {{
            $result.matched = $false
        }} else {{
            $result.matched = $true
            $children = @(Get-DescendantProcessIds -RootPid $PidToKill | Sort-Object -Descending -Unique)
            foreach ($childPid in $children) {{
                try {{
                    Stop-Process -Id $childPid -Force -ErrorAction Stop
                }} catch {{
                }}
            }}
            Stop-Process -Id $PidToKill -Force -ErrorAction Stop
            $result.killed = $true
            $result.children = @($children)
        }}
    }}
}} catch {{
    $result.error = $_.Exception.Message
}}

$result | ConvertTo-Json -Compress
""".strip()


def cleanup_stale_windows_validator(
    host: str,
    pid: int,
    started_at: str,
    *,
    ps_literal_fn: Callable[[str], str],
    run_logged_command_fn: Callable,
    windows_ssh_powershell_command_fn: Callable[[str], list[str]],
    trim_line_fn: Callable[[str], str],
) -> dict:
    ps_script = stale_windows_validator_cleanup_script(
        pid,
        started_at,
        ps_literal_fn=ps_literal_fn,
    )
    run = run_logged_command_fn(
        windows_ssh_powershell_command_fn(host),
        input_text=ps_script,
        timeout=120,
    )
    lines = [line.strip() for line in run.get("output", "").splitlines() if line.strip()]
    payload = {}
    if lines:
        try:
            payload = json.loads(lines[-1])
        except json.JSONDecodeError:
            payload = {"error": trim_line_fn(lines[-1])}
    if run.get("returncode") != 0:
        payload.setdefault("error", f"cleanup command exited {run.get('returncode')}")
    return payload


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
