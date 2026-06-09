"""Windows SSH/PowerShell probe helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import base64
import json
from pathlib import Path
import re
import subprocess


def ps_literal(value: str) -> str:
    return value.replace("'", "''")


_SAFE_CI_BRANCH_RE = re.compile(r"^[A-Za-z0-9._/-]+$")


def validate_ci_branch_name(branch: str) -> str:
    normalized = (branch or "").strip()
    if not normalized:
        raise ValueError("CI branch name is required")
    if not _SAFE_CI_BRANCH_RE.fullmatch(normalized):
        raise ValueError(
            "Unsupported branch name for local-ci transport. "
            "Use letters, numbers, dot, underscore, slash, or hyphen only."
        )
    return normalized


def windows_ssh_powershell_command(host: str) -> list[str]:
    # `powershell -Command -` silently no-ops some multi-line try/finally scripts on WinRM/OpenSSH.
    # Read stdin explicitly and invoke it so complex validation scripts execute reliably.
    return [
        "ssh",
        host,
        "powershell",
        "-NoProfile",
        "-NonInteractive",
        "-Command",
        "$script = [Console]::In.ReadToEnd(); Invoke-Expression $script",
    ]


def run_windows_ssh_powershell(
    host: str,
    ps_script: str,
    *,
    timeout: int = 60,
    run_ssh_subprocess_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> subprocess.CompletedProcess[str]:
    return run_ssh_subprocess_fn(
        windows_ssh_powershell_command(host),
        input=ps_script,
        timeout=timeout,
    )


def parse_windows_ssh_json(stdout: str) -> dict:
    for line in reversed(stdout.splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(payload, dict):
            raise RuntimeError("Windows SSH script returned a non-object JSON payload")
        return payload
    raise RuntimeError("Windows SSH script returned no JSON payload")


def windows_contract_expand_expression(raw_value: str, *, ps_literal_fn: Callable[[str], str] = ps_literal) -> str:
    return f"[Environment]::ExpandEnvironmentVariables('{ps_literal_fn(raw_value)}')"


def windows_session_agent_template_path(script_dir: Path) -> Path:
    return script_dir / "windows_session_agent.ps1"


def windows_ssh_write_text(
    host: str,
    remote_path: str,
    content: str,
    *,
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    parse_windows_ssh_json_fn: Callable[[str], dict] = parse_windows_ssh_json,
    windows_contract_expand_expression_fn: Callable[[str], str] = windows_contract_expand_expression,
    ps_literal_fn: Callable[[str], str] = ps_literal,
) -> None:
    payload = base64.b64encode(content.encode("utf-8")).decode("ascii")
    ps_script = f"""
$RawPath = '{ps_literal_fn(remote_path)}'
$ExpandedPath = {windows_contract_expand_expression_fn(remote_path)}
$Parent = Split-Path -Parent $ExpandedPath
if ($Parent) {{
    New-Item -ItemType Directory -Path $Parent -Force | Out-Null
}}
$Bytes = [Convert]::FromBase64String('{payload}')
[System.IO.File]::WriteAllBytes($ExpandedPath, $Bytes)
$result = @{{
    path = $ExpandedPath
    exists = Test-Path $ExpandedPath
}}
$result | ConvertTo-Json -Compress
"""
    run = run_windows_ssh_powershell_fn(host, ps_script, timeout=120)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"write exited {run.returncode}"
        raise RuntimeError(detail)
    payload_json = parse_windows_ssh_json_fn(run.stdout)
    if not payload_json.get("exists"):
        raise RuntimeError(f"Remote write failed for `{remote_path}`")


def windows_ssh_fetch_file(
    host: str,
    remote_path: str,
    local_path: Path,
    *,
    optional: bool = False,
    timeout: int = 60,
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    windows_contract_expand_expression_fn: Callable[[str], str] = windows_contract_expand_expression,
) -> bool:
    ps_script = f"""
$ExpandedPath = {windows_contract_expand_expression_fn(remote_path)}
if (-not (Test-Path $ExpandedPath)) {{
    Write-Output '__PULP_MISSING__'
    exit 0
}}
$Bytes = [System.IO.File]::ReadAllBytes($ExpandedPath)
[Console]::Out.WriteLine([Convert]::ToBase64String($Bytes))
"""
    run = run_windows_ssh_powershell_fn(host, ps_script, timeout=timeout)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"fetch exited {run.returncode}"
        if optional:
            return False
        raise RuntimeError(detail)
    output = "".join(line.strip() for line in run.stdout.splitlines() if line.strip())
    if output == "__PULP_MISSING__":
        return False if optional else (_ for _ in ()).throw(RuntimeError(f"Remote file `{remote_path}` does not exist"))
    local_path.parent.mkdir(parents=True, exist_ok=True)
    local_path.write_bytes(base64.b64decode(output.encode("ascii")))
    return True


def windows_ssh_read_json(
    host: str,
    remote_path: str,
    *,
    timeout: int = 30,
    optional: bool = False,
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    windows_contract_expand_expression_fn: Callable[[str], str] = windows_contract_expand_expression,
) -> dict | None:
    ps_script = f"""
$ExpandedPath = {windows_contract_expand_expression_fn(remote_path)}
if (-not (Test-Path $ExpandedPath)) {{
    Write-Output '__PULP_MISSING__'
    exit 0
}}
Get-Content -LiteralPath $ExpandedPath -Raw
"""
    run = run_windows_ssh_powershell_fn(host, ps_script, timeout=timeout)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"read exited {run.returncode}"
        if optional:
            return None
        raise RuntimeError(detail)
    output = run.stdout.strip()
    if output == "__PULP_MISSING__":
        return None if optional else (_ for _ in ()).throw(RuntimeError(f"Remote JSON `{remote_path}` does not exist"))
    return json.loads(output)


def windows_ssh_remove_path(
    host: str,
    remote_path: str,
    *,
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    windows_contract_expand_expression_fn: Callable[[str], str] = windows_contract_expand_expression,
) -> None:
    ps_script = f"""
$ExpandedPath = {windows_contract_expand_expression_fn(remote_path)}
if (Test-Path $ExpandedPath) {{
    Remove-Item -LiteralPath $ExpandedPath -Recurse -Force
}}
"""
    try:
        run_windows_ssh_powershell_fn(host, ps_script, timeout=30)
    except Exception:
        pass


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
