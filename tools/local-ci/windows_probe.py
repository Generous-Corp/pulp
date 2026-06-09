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


def probe_windows_repo_checkout(
    host: str,
    repo_path: str | None,
    *,
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    windows_repo_path_is_unsafe_fn: Callable[[str | None, str | None], bool],
    parse_windows_ssh_json_fn: Callable[[str], dict] = parse_windows_ssh_json,
    ps_literal_fn: Callable[[str], str] = ps_literal,
) -> dict:
    raw_repo = repo_path or ""
    ps_script = f"""
$RepoRaw = '{ps_literal_fn(raw_repo)}'
$Repo = if ($RepoRaw) {{ [Environment]::ExpandEnvironmentVariables($RepoRaw) }} else {{ '' }}
$RepoExists = $false
$GitDirExists = $false
$HasOrigin = $false
$OriginUrl = ''
$Head = ''
$HeadExists = $false
$SetupPath = ''
$SetupExists = $false
if ($Repo) {{
    $RepoExists = [bool](Test-Path $Repo)
    $SetupPath = [string](Join-Path $Repo 'setup.sh')
    $SetupExists = [bool](Test-Path $SetupPath)
}}
if ($Repo -and (Test-Path (Join-Path $Repo '.git'))) {{
    $GitDirExists = $true
    $HasOrigin = [bool]((git -C $Repo remote 2>$null) | Where-Object {{ $_ -eq 'origin' }} | Select-Object -First 1)
    if ($HasOrigin) {{
        $OriginUrl = [string]((git -C $Repo remote get-url origin 2>$null) | Select-Object -First 1)
    }}
    $Head = [string]((git -C $Repo rev-parse --verify --quiet HEAD 2>$null) | Select-Object -First 1)
    $HeadExists = -not [string]::IsNullOrWhiteSpace($Head)
}}
$result = @{{
    home_dir = [string]$HOME
    repo_path_raw = [string]$RepoRaw
    repo_path = [string]$Repo
    repo_exists = $RepoExists
    git_dir_exists = $GitDirExists
    head = [string]$Head
    head_exists = $HeadExists
    setup_path = [string]$SetupPath
    setup_exists = $SetupExists
    origin_url = [string]$OriginUrl
}}
$result | ConvertTo-Json -Compress
"""
    run = run_windows_ssh_powershell_fn(host, ps_script, timeout=60)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"repo probe exited {run.returncode}"
        raise RuntimeError(detail)
    result = parse_windows_ssh_json_fn(run.stdout)
    result["repo_path_unsafe"] = windows_repo_path_is_unsafe_fn(result.get("repo_path"), result.get("home_dir"))
    return result


def ensure_windows_remote_repo_checkout(
    host: str,
    repo_path: str | None,
    *,
    remote_url: str | None,
    bundle_name: str | None,
    bundle_ref: str | None,
    probe_windows_repo_checkout_fn: Callable[[str, str | None], dict],
    windows_repo_path_is_unsafe_fn: Callable[[str | None, str | None], bool],
    windows_default_repo_checkout_path_fn: Callable[[str | None], str],
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    parse_windows_ssh_json_fn: Callable[[str], dict] = parse_windows_ssh_json,
    windows_contract_expand_expression_fn: Callable[[str], str] = windows_contract_expand_expression,
    ps_literal_fn: Callable[[str], str] = ps_literal,
) -> dict:
    probe = probe_windows_repo_checkout_fn(host, repo_path)
    if not isinstance(probe, dict):
        raise RuntimeError("Windows repo probe returned no structured payload")
    effective_repo_path = probe.get("repo_path") or (repo_path or "").strip()
    home_dir = probe.get("home_dir") or ""
    if windows_repo_path_is_unsafe_fn(effective_repo_path, home_dir):
        effective_repo_path = windows_default_repo_checkout_path_fn(home_dir)
    needs_materialize = not (bool(probe.get("head_exists")) and bool(probe.get("setup_exists")))

    ps_script = f"""
$ErrorActionPreference = 'Stop'
$Repo = {windows_contract_expand_expression_fn(effective_repo_path)}
$RemoteUrl = '{ps_literal_fn(remote_url or "")}'
$Bundle = '{ps_literal_fn(bundle_name or "")}'
$BundleRef = '{ps_literal_fn(bundle_ref or "")}'
$NeedsMaterialize = {"$true" if needs_materialize else "$false"}
$PreviousLfsSkipSmudge = [Environment]::GetEnvironmentVariable('GIT_LFS_SKIP_SMUDGE', 'Process')
[Environment]::SetEnvironmentVariable('GIT_LFS_SKIP_SMUDGE', '1', 'Process')
New-Item -ItemType Directory -Force -Path $Repo | Out-Null
if (-not (Test-Path (Join-Path $Repo '.git'))) {{
    & git init $Repo | Out-Null
    if ($LASTEXITCODE -ne 0) {{
        throw ('git init failed for ' + $Repo)
    }}
}}
$HasOrigin = [bool]((git -C $Repo remote 2>$null) | Where-Object {{ $_ -eq 'origin' }} | Select-Object -First 1)
$OriginUrl = ''
if ($HasOrigin) {{
    $OriginUrl = [string]((git -C $Repo remote get-url origin 2>$null) | Select-Object -First 1)
}}
if (-not $OriginUrl -and $RemoteUrl) {{
    & git -C $Repo remote add origin $RemoteUrl | Out-Null
    if ($LASTEXITCODE -ne 0) {{
        throw ('git remote add origin failed for ' + $Repo)
    }}
    $OriginUrl = $RemoteUrl
}}
$Head = ''
$HeadExists = $false
$SetupPath = [string](Join-Path $Repo 'setup.sh')
$SetupExists = [bool](Test-Path $SetupPath)
if ($NeedsMaterialize) {{
    if ($Bundle -and $BundleRef) {{
        $BundlePath = Join-Path $HOME $Bundle
        & git -C $Repo fetch $BundlePath "$BundleRef`:refs/pulp-ci-bundles/source" | Out-Null
        if ($LASTEXITCODE -ne 0) {{
            throw ('git fetch bundle failed for ' + $Repo)
        }}
        if (Test-Path $BundlePath) {{ Remove-Item -LiteralPath $BundlePath -Force }}
    }} elseif ($RemoteUrl) {{
        & git -C $Repo fetch --depth 1 origin main | Out-Null
        if ($LASTEXITCODE -ne 0) {{
            throw ('git fetch origin main failed for ' + $Repo)
        }}
    }}
    if (($Bundle -and $BundleRef) -or $RemoteUrl) {{
        & git -C $Repo checkout --force -B main FETCH_HEAD | Out-Null
        if ($LASTEXITCODE -ne 0) {{
            throw ('git checkout main failed for ' + $Repo)
        }}
        $SetupExists = [bool](Test-Path $SetupPath)
    }}
}}
[Environment]::SetEnvironmentVariable('GIT_LFS_SKIP_SMUDGE', $PreviousLfsSkipSmudge, 'Process')
if (Test-Path (Join-Path $Repo '.git')) {{
    $Head = [string]((git -C $Repo rev-parse --verify --quiet HEAD 2>$null) | Select-Object -First 1)
    $HeadExists = -not [string]::IsNullOrWhiteSpace($Head)
}}
$result = @{{
    home_dir = [string]$HOME
    repo_path = [string]$Repo
    repo_exists = [bool](Test-Path $Repo)
    git_dir_exists = [bool](Test-Path (Join-Path $Repo '.git'))
    head = [string]$Head
    head_exists = $HeadExists
    setup_path = [string]$SetupPath
    setup_exists = $SetupExists
    origin_url = [string]$OriginUrl
}}
$result | ConvertTo-Json -Compress
"""
    run = run_windows_ssh_powershell_fn(host, ps_script, timeout=120)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"repo bootstrap exited {run.returncode}"
        raise RuntimeError(detail)
    result = parse_windows_ssh_json_fn(run.stdout)
    if not isinstance(result, dict):
        raise RuntimeError("Windows repo bootstrap returned no structured payload")
    result["repo_path_unsafe"] = windows_repo_path_is_unsafe_fn(result.get("repo_path"), result.get("home_dir"))
    return result


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


def probe_windows_remote_tooling(
    host: str,
    *,
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    parse_windows_ssh_json_fn: Callable[[str], dict] = parse_windows_ssh_json,
) -> dict:
    ps_script = r"""
$gitCmd = Get-Command git -ErrorAction SilentlyContinue
$ghCmd = Get-Command gh -ErrorAction SilentlyContinue
$wingetCmd = Get-Command winget -ErrorAction SilentlyContinue

$gitVersion = ''
if ($gitCmd) {
    $gitVersion = ((& $gitCmd.Source --version 2>$null) | Select-Object -First 1)
}

$ghVersion = ''
$ghAuthReady = $null
$ghAuthDetail = ''
if ($ghCmd) {
    $ghVersion = ((& $ghCmd.Source --version 2>$null) | Select-Object -First 1)
    $ghAuthOutput = (& $ghCmd.Source auth status 2>&1)
    $ghAuthReady = ($LASTEXITCODE -eq 0)
    $ghAuthDetail = (($ghAuthOutput | Select-Object -First 4) -join ' | ')
}

$wingetVersion = ''
if ($wingetCmd) {
    $wingetVersion = ((& $wingetCmd.Source --version 2>$null) | Select-Object -First 1)
}

$gitPath = ''
if ($gitCmd) {
    $gitPath = [string]$gitCmd.Source
}

$ghPath = ''
if ($ghCmd) {
    $ghPath = [string]$ghCmd.Source
}

$wingetPath = ''
if ($wingetCmd) {
    $wingetPath = [string]$wingetCmd.Source
}

$result = @{
    git_found = [bool]$gitCmd
    git_path = $gitPath
    git_version = [string]$gitVersion
    gh_found = [bool]$ghCmd
    gh_path = $ghPath
    gh_version = [string]$ghVersion
    gh_auth_ready = $ghAuthReady
    gh_auth_detail = [string]$ghAuthDetail
    winget_found = [bool]$wingetCmd
    winget_path = $wingetPath
    winget_version = [string]$wingetVersion
}
$result | ConvertTo-Json -Compress
"""
    run = run_windows_ssh_powershell_fn(host, ps_script, timeout=60)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"tooling probe exited {run.returncode}"
        raise RuntimeError(detail)
    return parse_windows_ssh_json_fn(run.stdout)


def install_windows_remote_tool(
    host: str,
    package_id: str,
    *,
    timeout: int = 900,
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    ps_literal_fn: Callable[[str], str] = ps_literal,
) -> None:
    ps_script = f"""
$Winget = Get-Command winget -ErrorAction SilentlyContinue
if (-not $Winget) {{
    throw 'winget not found'
}}
$PackageId = '{ps_literal_fn(package_id)}'
$InstallArgs = @(
    'install',
    '--id',
    $PackageId,
    '-e',
    '--source',
    'winget',
    '--accept-package-agreements',
    '--accept-source-agreements',
    '--disable-interactivity'
)
& $Winget.Source @InstallArgs
if ($LASTEXITCODE -ne 0) {{
    throw ('winget install failed for ' + $PackageId + ' with exit code ' + $LASTEXITCODE)
}}
"""
    run = run_windows_ssh_powershell_fn(host, ps_script, timeout=timeout)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"winget install exited {run.returncode}"
        raise RuntimeError(detail)


def ensure_windows_remote_tooling(
    host: str,
    *,
    install_optional: bool,
    required_tools: dict,
    optional_tools: dict,
    probe_windows_remote_tooling_fn: Callable[[str], dict],
    install_windows_remote_tool_fn: Callable[..., None],
) -> dict:
    probe = probe_windows_remote_tooling_fn(host)
    installed: list[str] = []

    for tool_name, spec in required_tools.items():
        if probe.get(f"{tool_name}_found"):
            continue
        if not probe.get("winget_found"):
            raise RuntimeError(
                f"`{tool_name}` is missing on the Windows target and `winget` is unavailable; "
                "install it manually, then rerun `pulp ci-local desktop install windows`"
            )
        install_windows_remote_tool_fn(host, spec["winget_id"])
        installed.append(tool_name)
        probe = probe_windows_remote_tooling_fn(host)
        if not probe.get(f"{tool_name}_found"):
            raise RuntimeError(
                f"`{tool_name}` is still missing after `winget` install; "
                "verify PATH on the Windows target, then rerun `pulp ci-local desktop doctor windows`"
            )

    if install_optional:
        for tool_name, spec in optional_tools.items():
            if probe.get(f"{tool_name}_found") or not probe.get("winget_found"):
                continue
            try:
                install_windows_remote_tool_fn(host, spec["winget_id"])
                installed.append(tool_name)
                probe = probe_windows_remote_tooling_fn(host)
            except RuntimeError:
                # Optional tools are advisory. Keep the required setup path resilient.
                pass

    return {"probe": probe, "installed": installed}


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
