"""Core Windows SSH/PowerShell helper primitives."""
from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
from collections.abc import Callable


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
