"""Remote file helpers for Windows SSH/PowerShell local-CI targets."""
from __future__ import annotations

from collections.abc import Callable
import base64
import json
from pathlib import Path
import subprocess

from windows_probe_core import parse_windows_ssh_json, windows_contract_expand_expression, ps_literal


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
