"""Generic git subprocess helpers for local CI."""

from __future__ import annotations

import subprocess
from pathlib import Path


def run_git(
    args: list[str],
    *,
    cwd: Path,
    check: bool = True,
    run_fn=None,
) -> subprocess.CompletedProcess:
    run_fn = run_fn or subprocess.run
    run = run_fn(
        ["git", *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
    )
    if check and run.returncode != 0:
        detail = (run.stderr or run.stdout or "").strip()
        raise RuntimeError(f"git {' '.join(args)} failed: {detail or run.returncode}")
    return run
