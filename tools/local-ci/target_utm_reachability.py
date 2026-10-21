"""UTM fallback reachability helpers for local CI targets."""

from __future__ import annotations

from collections.abc import Callable
import subprocess


# UTM's control socket can remain alive while the frontend is wedged.  Without
# a bound here, a reachability probe can hold the local-CI scheduler forever and
# prevent its SSH fallback from running.
UTMCTL_TIMEOUT_SECS = 10


def _utmctl(
    command: list[str],
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> subprocess.CompletedProcess[str] | None:
    try:
        return run_fn(
            command,
            capture_output=True,
            text=True,
            timeout=UTMCTL_TIMEOUT_SECS,
        )
    except (OSError, subprocess.TimeoutExpired):
        # A wedged or unavailable UTM control plane is a typed unreachable
        # result.  The caller can then report the host as unavailable and use
        # its normal fallback policy instead of hanging indefinitely.
        return None


def utmctl_vm_status(
    vm_name: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> str | None:
    result = _utmctl(["utmctl", "list"], run_fn=run_fn)
    if result is None or result.returncode != 0:
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
    result = _utmctl(["utmctl", "start", vm_name], run_fn=run_fn)
    return result is not None and result.returncode == 0
