"""UTM fallback reachability helpers for local CI targets."""

from __future__ import annotations

from collections.abc import Callable
import subprocess


def utmctl_vm_status(
    vm_name: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> str | None:
    result = run_fn(["utmctl", "list"], capture_output=True, text=True)
    if result.returncode != 0:
        return None
    for line in result.stdout.splitlines():
        if vm_name in line:
            parts = line.split()
            if len(parts) >= 2:
                return parts[1]
    return None


def utmctl_start(
    vm_name: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> bool:
    result = run_fn(["utmctl", "start", vm_name], capture_output=True, text=True)
    return result.returncode == 0
