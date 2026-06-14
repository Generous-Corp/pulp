"""Windows session-agent bootstrap helper."""
from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import subprocess

from windows_probe_core import parse_windows_ssh_json, ps_literal, windows_contract_expand_expression


def bootstrap_windows_session_agent(
    host: str,
    contract: dict,
    *,
    windows_session_agent_template_path_fn: Callable[[], Path],
    windows_ssh_write_text_fn: Callable[[str, str, str], None],
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    parse_windows_ssh_json_fn: Callable[[str], dict] = parse_windows_ssh_json,
    windows_contract_expand_expression_fn: Callable[[str], str] = windows_contract_expand_expression,
    ps_literal_fn: Callable[[str], str] = ps_literal,
) -> dict:
    script_path = windows_session_agent_template_path_fn()
    if not script_path.exists():
        raise RuntimeError(f"Windows session agent template missing: {script_path}")
    windows_ssh_write_text_fn(host, contract["script_path"], script_path.read_text())
    ps_script = f"""
$TaskName = '{ps_literal_fn(contract["task_name"])}'
$RemoteRootRaw = '{ps_literal_fn(contract["remote_root"])}'
$ScriptPathRaw = '{ps_literal_fn(contract["script_path"])}'
$RemoteRoot = {windows_contract_expand_expression_fn(contract["remote_root"])}
$ScriptPath = {windows_contract_expand_expression_fn(contract["script_path"])}
$JobsDir = Join-Path $RemoteRoot 'jobs'
$ResultsDir = Join-Path $RemoteRoot 'results'
$LogsDir = Join-Path $RemoteRoot 'logs'
New-Item -ItemType Directory -Path $RemoteRoot -Force | Out-Null
New-Item -ItemType Directory -Path $JobsDir -Force | Out-Null
New-Item -ItemType Directory -Path $ResultsDir -Force | Out-Null
New-Item -ItemType Directory -Path $LogsDir -Force | Out-Null
$ActionArgument = '-NoProfile -ExecutionPolicy Bypass -File "{{0}}" -RemoteRoot "{{1}}"' -f $ScriptPath, $RemoteRootRaw
$Action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $ActionArgument
$Trigger = New-ScheduledTaskTrigger -AtLogOn
$Principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
$SettingsArgs = @{{
    AllowStartIfOnBatteries = $true
    DontStopIfGoingOnBatteries = $true
    MultipleInstances = 'IgnoreNew'
    ExecutionTimeLimit = New-TimeSpan -Minutes 30
}}
$Settings = New-ScheduledTaskSettingsSet @SettingsArgs
Register-ScheduledTask -TaskName $TaskName -Action $Action -Trigger $Trigger -Principal $Principal -Settings $Settings -Force | Out-Null
$task = Get-ScheduledTask -TaskName $TaskName -ErrorAction Stop
$TaskState = ''
if ($task) {{
    $TaskState = [string]$task.State
}}
$result = @{{
    task_name = $TaskName
    task_present = [bool]$task
    task_state = $TaskState
    remote_root = $RemoteRoot
    script_path = $ScriptPath
    script_exists = Test-Path $ScriptPath
    jobs_dir = $JobsDir
    jobs_dir_exists = Test-Path $JobsDir
    results_dir = $ResultsDir
    results_dir_exists = Test-Path $ResultsDir
    logs_dir = $LogsDir
    logs_dir_exists = Test-Path $LogsDir
}}
$result | ConvertTo-Json -Compress
"""
    run = run_windows_ssh_powershell_fn(host, ps_script, timeout=120)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"bootstrap exited {run.returncode}"
        raise RuntimeError(detail)
    return parse_windows_ssh_json_fn(run.stdout)


__all__ = ["bootstrap_windows_session_agent"]
