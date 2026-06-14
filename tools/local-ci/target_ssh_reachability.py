"""SSH target reachability helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import shlex
import subprocess


def ssh_probe(
    host: str,
    timeout: int = 5,
    *,
    run_ssh_subprocess_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> subprocess.CompletedProcess[str]:
    cmd = ["ssh", "-o", f"ConnectTimeout={timeout}", "-o", "BatchMode=yes", host, "echo", "up"]
    try:
        return run_ssh_subprocess_fn(
            cmd,
            timeout=max(timeout, 5),
        )
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124, "", f"SSH probe timed out after {max(timeout, 5)}s")


def ssh_reachable(
    host: str,
    timeout: int = 5,
    *,
    ssh_probe_fn: Callable[[str, int], subprocess.CompletedProcess[str]],
) -> bool:
    return ssh_probe_fn(host, timeout).returncode == 0


def ssh_failure_detail(
    host: str,
    timeout: int = 5,
    *,
    ssh_probe_fn: Callable[[str, int], subprocess.CompletedProcess[str]],
) -> str:
    result = ssh_probe_fn(host, timeout)
    stderr = (result.stderr or "").strip()
    if "timed out" in stderr.lower():
        return f"{host} (connection timed out; verify network reachability and SSH service on the target)"
    if "Connection reset by peer" in stderr or "kex_exchange_identification" in stderr:
        return f"{host} (SSH service reset during handshake; verify OpenSSH server on the target)"
    if "Connection refused" in stderr:
        return f"{host} (connection refused; verify the SSH service is running on the target)"
    if stderr:
        first_line = stderr.splitlines()[0].strip()
        return f"{host} ({first_line})"
    return f"{host} (unreachable; verify SSH access from this host)"


def ssh_command_result(
    host: str,
    remote_cmd: str,
    *,
    timeout: int = 30,
    run_ssh_subprocess_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> subprocess.CompletedProcess[str]:
    prefixed_cmd = 'export PATH="$HOME/.local/bin:$PATH"; ' + remote_cmd
    return run_ssh_subprocess_fn(
        ["ssh", "-o", f"ConnectTimeout={max(5, min(timeout, 30))}", host, "bash", "-lc", shlex.quote(prefixed_cmd)],
        timeout=timeout,
    )
