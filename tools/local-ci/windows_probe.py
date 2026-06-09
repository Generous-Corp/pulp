"""Windows SSH/PowerShell probe helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import json
import subprocess


def ps_literal(value: str) -> str:
    return value.replace("'", "''")


def windows_contract_expand_expression(
    raw_value: str,
    *,
    ps_literal_fn: Callable[[str], str] = ps_literal,
) -> str:
    return f"[Environment]::ExpandEnvironmentVariables('{ps_literal_fn(raw_value)}')"


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
    timeout: int,
    run_ssh_subprocess_fn: Callable[..., subprocess.CompletedProcess[str]],
    windows_ssh_powershell_command_fn: Callable[[str], list[str]],
) -> subprocess.CompletedProcess[str]:
    return run_ssh_subprocess_fn(
        windows_ssh_powershell_command_fn(host),
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


def probe_windows_repo_checkout(
    host: str,
    repo_path: str | None,
    *,
    ps_literal_fn: Callable[[str], str],
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    parse_windows_ssh_json_fn: Callable[[str], dict],
    windows_repo_path_is_unsafe_fn: Callable[[str | None, str | None], bool],
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
    windows_contract_expand_expression_fn: Callable[[str], str],
    ps_literal_fn: Callable[[str], str],
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    parse_windows_ssh_json_fn: Callable[[str], dict],
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
    ps_literal_fn: Callable[[str], str],
    windows_contract_expand_expression_fn: Callable[[str], str],
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
    parse_windows_ssh_json_fn: Callable[[str], dict],
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
        $match = [regex]::Match($line, '^\\s*>?\\s*(?<username>\\S+)\\s+(?:(?<sessionname>\\S+)\\s+)?(?<id>\\d+)\\s+(?<state>Active|Disc|Disconnected|Conn|Listen|Idle|Down|Init)\\b')
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
    parse_windows_ssh_json_fn: Callable[[str], dict],
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
    timeout: int,
    ps_literal_fn: Callable[[str], str],
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> None:
    ps_script = f"""
$Winget = Get-Command winget -ErrorAction SilentlyContinue
if (-not $Winget) {{
    throw 'winget not found'
}}
& $Winget.Source install --id '{ps_literal_fn(package_id)}' -e --source winget --accept-package-agreements --accept-source-agreements --disable-interactivity
if ($LASTEXITCODE -ne 0) {{
    throw ('winget install failed for {ps_literal_fn(package_id)} with exit code ' + $LASTEXITCODE)
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
    probe_windows_remote_tooling_fn: Callable[[str], dict],
    install_windows_remote_tool_fn: Callable[..., None],
    required_tools: dict,
    optional_tools: dict,
) -> dict:
    probe = probe_windows_remote_tooling_fn(host)
    installed: list[str] = []

    for tool_name, spec in required_tools.items():
        if probe.get(f"{tool_name}_found"):
            continue
        if not probe.get("winget_found"):
            raise RuntimeError(
                f"`{tool_name}` is missing on the Windows target and `winget` is unavailable; install it manually, then rerun `pulp ci-local desktop install windows`"
            )
        install_windows_remote_tool_fn(host, spec["winget_id"])
        installed.append(tool_name)
        probe = probe_windows_remote_tooling_fn(host)
        if not probe.get(f"{tool_name}_found"):
            raise RuntimeError(
                f"`{tool_name}` is still missing after `winget` install; verify PATH on the Windows target, then rerun `pulp ci-local desktop doctor windows`"
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
