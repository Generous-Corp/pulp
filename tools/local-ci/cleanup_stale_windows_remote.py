"""Remote stale Windows validator cleanup execution."""

from __future__ import annotations

import json
from collections.abc import Callable


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


__all__ = [
    "cleanup_stale_windows_validator",
    "stale_windows_validator_cleanup_script",
]
