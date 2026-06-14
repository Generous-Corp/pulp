"""Windows remote tooling probe/install helpers."""
from __future__ import annotations

from collections.abc import Callable
import subprocess

from windows_probe_core import parse_windows_ssh_json, ps_literal


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
