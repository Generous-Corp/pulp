"""Windows SSH/PowerShell probe helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path
import subprocess

from windows_probe_core import (
    parse_windows_ssh_json,
    ps_literal,
    run_windows_ssh_powershell,
    validate_ci_branch_name,
    windows_contract_expand_expression,
    windows_session_agent_template_path,
    windows_ssh_powershell_command,
)
from windows_remote_files import (
    windows_ssh_fetch_file,
    windows_ssh_read_json,
    windows_ssh_remove_path,
    windows_ssh_write_text,
)
from windows_repo_checkout import (
    ensure_windows_remote_repo_checkout,
    probe_windows_repo_checkout,
)
from windows_tooling import (
    ensure_windows_remote_tooling,
    install_windows_remote_tool,
    probe_windows_remote_tooling,
)


def probe_windows_session_agent(
    host: str,
    contract: dict,
    *,
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    parse_windows_ssh_json_fn: Callable[[str], dict] = parse_windows_ssh_json,
    windows_contract_expand_expression_fn: Callable[[str], str] = windows_contract_expand_expression,
    ps_literal_fn: Callable[[str], str] = ps_literal,
) -> dict:
    task_name = contract["task_name"]
    remote_root = contract["remote_root"]
    script_path = contract.get("script_path") or ""
    ps_script = f"""
$TaskName = '{ps_literal_fn(task_name)}'
$RemoteRootRaw = '{ps_literal_fn(remote_root)}'
$ScriptPathRaw = '{ps_literal_fn(script_path)}'
$RemoteRoot = {windows_contract_expand_expression_fn(remote_root)}
$ScriptPath = {windows_contract_expand_expression_fn(script_path)}
$task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
$activeUser = ''
try {{
    $activeUser = (Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).UserName
}} catch {{
    $activeUser = ''
}}
$loggedOnUser = ''
$loggedOnState = ''
$sessionRecords = @()
try {{
    $quserOutput = quser 2>$null
    foreach ($line in $quserOutput) {{
        if ([string]::IsNullOrWhiteSpace($line)) {{ continue }}
        if ($line -match 'USERNAME\\s+SESSIONNAME') {{ continue }}
        $SessionPattern = '^\\s*>?\\s*(?<username>\\S+)\\s+'
        $SessionPattern += '(?:(?<sessionname>\\S+)\\s+)?(?<id>\\d+)\\s+'
        $SessionPattern += '(?<state>Active|Disc|Disconnected|Conn|Listen|Idle|Down|Init)\\b'
        $match = [regex]::Match($line, $SessionPattern)
        if (-not $match.Success) {{ continue }}
        $record = @{{
            username = $match.Groups['username'].Value
            session_name = $match.Groups['sessionname'].Value
            session_id = $match.Groups['id'].Value
            state = $match.Groups['state'].Value
        }}
        $sessionRecords += $record
        if (-not $loggedOnUser -and @('Active', 'Disc', 'Disconnected') -contains $record.state) {{
            $loggedOnUser = $record.username
            $loggedOnState = $record.state
        }}
    }}
}} catch {{
}}
$TaskState = ''
if ($task) {{
    $TaskState = [string]$task.State
}}
$InteractiveUser = ''
if ($activeUser) {{
    $InteractiveUser = $activeUser
}} elseif ($loggedOnUser) {{
    $InteractiveUser = $loggedOnUser
}}
$result = @{{
    task_name = $TaskName
    task_present = [bool]$task
    task_state = $TaskState
    active_user = $activeUser
    logged_on_user = $loggedOnUser
    session_state = $loggedOnState
    session_records = $sessionRecords
    interactive_user = $InteractiveUser
    remote_root_raw = $RemoteRootRaw
    remote_root = $RemoteRoot
    agent_root_exists = Test-Path $RemoteRoot
    jobs_dir = Join-Path $RemoteRoot 'jobs'
    jobs_dir_exists = Test-Path (Join-Path $RemoteRoot 'jobs')
    results_dir = Join-Path $RemoteRoot 'results'
    results_dir_exists = Test-Path (Join-Path $RemoteRoot 'results')
    logs_dir = Join-Path $RemoteRoot 'logs'
    logs_dir_exists = Test-Path (Join-Path $RemoteRoot 'logs')
    script_path_raw = $ScriptPathRaw
    script_path = $ScriptPath
    script_exists = Test-Path $ScriptPath
}}
$result | ConvertTo-Json -Compress
"""
    run = run_windows_ssh_powershell_fn(host, ps_script, timeout=60)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"probe exited {run.returncode}"
        raise RuntimeError(detail)
    return parse_windows_ssh_json_fn(run.stdout)


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


def probe_windows_ssh_cmake_settings(
    host: str,
    cmake_generator: str,
    cmake_platform: str,
    cmake_generator_instance: str,
    *,
    windows_ssh_powershell_command_fn: Callable[[str], list[str]],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
    ps_literal_fn: Callable[[str], str] = ps_literal,
) -> tuple[str, str]:
    if cmake_platform and cmake_generator_instance:
        return cmake_platform, cmake_generator_instance

    ps_script = f"""
$RequestedPlatform = '{ps_literal_fn(cmake_platform)}'
$RequestedGeneratorInstance = '{ps_literal_fn(cmake_generator_instance)}'
$Generator = '{ps_literal_fn(cmake_generator)}'

function Resolve-CMakePlatform {{
    param([string]$Requested)
    if ($Requested) {{
        return $Requested
    }}
    if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {{
        return 'ARM64'
    }}
    return 'x64'
}}

function Resolve-VisualStudioInstance {{
    param([string]$Requested, [string]$Generator)
    if ($Requested) {{
        return $Requested
    }}
    if (-not $Generator -or -not $Generator.StartsWith('Visual Studio')) {{
        return ''
    }}
    $vswhere = Join-Path ${{env:ProgramFiles(x86)}} 'Microsoft Visual Studio\\Installer\\vswhere.exe'
    if (-not (Test-Path $vswhere)) {{
        return ''
    }}
    try {{
        $raw = (& $vswhere -latest -products * -format json) -join "`n"
        if (-not $raw) {{
            return ''
        }}
        $instances = $raw | ConvertFrom-Json
        if ($instances -isnot [System.Array]) {{
            $instances = @($instances)
        }}
        $preferred = $instances | Where-Object {{
            $_.productId -and $_.productId -ne 'Microsoft.VisualStudio.Product.BuildTools'
        }} | Select-Object -First 1
        if (-not $preferred) {{
            $preferred = $instances | Select-Object -First 1
        }}
        if ($preferred -and $preferred.installationPath) {{
            return $preferred.installationPath.Replace('\\', '/')
        }}
    }} catch {{
    }}
    return ''
}}

$resolved = @{{
    platform = Resolve-CMakePlatform $RequestedPlatform
    generator_instance = Resolve-VisualStudioInstance $RequestedGeneratorInstance $Generator
}}
$resolved | ConvertTo-Json -Compress
"""

    try:
        run = run_fn(
            windows_ssh_powershell_command_fn(host),
            input=ps_script,
            capture_output=True,
            text=True,
            timeout=60,
        )
    except (subprocess.SubprocessError, OSError):
        return cmake_platform, cmake_generator_instance

    if run.returncode != 0:
        return cmake_platform, cmake_generator_instance

    for line in reversed(run.stdout.splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            resolved = json.loads(line)
        except json.JSONDecodeError:
            continue
        return (
            resolved.get("platform") or cmake_platform,
            resolved.get("generator_instance") or cmake_generator_instance,
        )
    return cmake_platform, cmake_generator_instance
