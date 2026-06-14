"""SSH subprocess retry helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import subprocess
import time


SSH_TRANSIENT_PATTERNS = (
    "Connection reset by peer",
    "kex_exchange_identification",
    "Connection closed by remote host",
    "Connection timed out during banner exchange",
    "ssh_exchange_identification",
)


def is_transient_ssh_failure_detail(detail: str) -> bool:
    text = detail or ""
    return any(pattern in text for pattern in SSH_TRANSIENT_PATTERNS)


def run_ssh_subprocess(
    args: list[str],
    *,
    input: str | None = None,
    timeout: int | None = None,
    retries: int = 3,
    retry_delay_secs: float = 2.0,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    sleep_fn: Callable[[float], None] = time.sleep,
) -> subprocess.CompletedProcess[str]:
    attempt = 0
    while True:
        attempt += 1
        result = run_fn(
            args,
            input=input,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        detail = "\n".join(part for part in [result.stderr.strip(), result.stdout.strip()] if part)
        if result.returncode == 0 or attempt >= retries or not is_transient_ssh_failure_detail(detail):
            return result
        sleep_fn(retry_delay_secs * attempt)
