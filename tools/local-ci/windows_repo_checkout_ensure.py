"""Windows remote repository checkout materialization helper."""
from __future__ import annotations

from collections.abc import Callable
import subprocess

from windows_probe_core import parse_windows_ssh_json, ps_literal, windows_contract_expand_expression


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


__all__ = ["ensure_windows_remote_repo_checkout"]
