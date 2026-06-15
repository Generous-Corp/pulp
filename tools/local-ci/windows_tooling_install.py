"""Windows remote tooling install helper."""
from __future__ import annotations

from collections.abc import Callable
import subprocess

from windows_probe_core import ps_literal


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


__all__ = ["install_windows_remote_tool"]
