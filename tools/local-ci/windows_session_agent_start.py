"""Windows session-agent scheduled-task start helper."""
from __future__ import annotations

from collections.abc import Callable
import subprocess

from windows_probe_core import parse_windows_ssh_json, ps_literal


def start_windows_session_agent_task(
    host: str,
    contract: dict,
    *,
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    parse_windows_ssh_json_fn: Callable[[str], dict] = parse_windows_ssh_json,
    ps_literal_fn: Callable[[str], str] = ps_literal,
) -> None:
    ps_script = f"""
$TaskName = '{ps_literal_fn(contract["task_name"])}'
Start-ScheduledTask -TaskName $TaskName
$result = @{{
    started = $true
    task_name = $TaskName
}}
$result | ConvertTo-Json -Compress
"""
    run = run_windows_ssh_powershell_fn(host, ps_script, timeout=30)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"start exited {run.returncode}"
        raise RuntimeError(detail)
    parse_windows_ssh_json_fn(run.stdout)


__all__ = ["start_windows_session_agent_task"]
