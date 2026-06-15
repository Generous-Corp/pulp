"""User notification helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import subprocess


def notify(
    message: str,
    *,
    print_fn: Callable[..., object] = print,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> None:
    print_fn("\a", end="", flush=True)
    try:
        run_fn(
            ["osascript", "-e", f'display notification "{message}" with title "Pulp CI"'],
            capture_output=True,
            timeout=5,
        )
    except Exception:
        pass
