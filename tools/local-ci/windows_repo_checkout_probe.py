"""Windows remote repository checkout probe helper."""
from __future__ import annotations

from collections.abc import Callable
import subprocess

from windows_probe_core import parse_windows_ssh_json, ps_literal


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


__all__ = ["probe_windows_repo_checkout"]
