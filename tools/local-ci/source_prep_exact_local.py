"""Local exact-SHA worktree primitives for desktop source preparation."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import shutil
import subprocess


def local_worktree_matches(path: Path, sha: str, *, run_fn: Callable[..., subprocess.CompletedProcess]) -> bool:
    if not (path / ".git").exists():
        return False
    result = run_fn(
        ["git", "-C", str(path), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=False,
    )
    return result.returncode == 0 and result.stdout.strip() == sha


def reset_local_worktree(
    path: Path,
    *,
    root: Path,
    run_fn: Callable[..., subprocess.CompletedProcess],
    rmtree_fn: Callable[..., None] = shutil.rmtree,
) -> None:
    run_fn(
        ["git", "worktree", "remove", "--force", str(path)],
        cwd=root,
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    rmtree_fn(path, ignore_errors=True)
    run_fn(
        ["git", "worktree", "prune"],
        cwd=root,
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
