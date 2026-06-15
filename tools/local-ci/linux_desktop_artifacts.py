"""Linux desktop SSH artifact transfer helpers."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import subprocess


def fetch_ssh_artifact(
    host: str,
    remote_path: str,
    local_path: Path,
    *,
    optional: bool = False,
    timeout: int = 60,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> bool:
    local_path.parent.mkdir(parents=True, exist_ok=True)
    result = run_fn(
        ["scp", f"{host}:{remote_path}", str(local_path)],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if result.returncode == 0 and local_path.exists():
        return True
    if optional:
        return False
    detail = result.stderr.strip() or result.stdout.strip() or f"scp exited {result.returncode}"
    raise RuntimeError(f"Failed to copy `{remote_path}` from {host}: {detail}")


def cleanup_remote_ssh_dir(
    host: str,
    remote_dir_expr: str,
    *,
    ssh_command_result_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> None:
    try:
        ssh_command_result_fn(host, f"rm -rf {remote_dir_expr}", timeout=20)
    except Exception:
        pass
