"""Git + time helpers for local CI.

Extracted from local_ci.py to give downstream modules (queue,
recovery, transport) a thin git-and-time seam without dragging in the
11k-line orchestrator. All six helpers shell out to git or return ISO
timestamps; nothing here touches local CI state files.

`ROOT` resolves to the repo root via parents[2] — that's the same
resolution local_ci.py uses, and it matches because both files sit
under tools/local-ci/.
"""

from __future__ import annotations

import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def current_branch() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def current_sha() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def git_root_for(path: Path) -> Path | None:
    result = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        cwd=path,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    return Path(result.stdout.strip()).resolve()


def resolve_git_ref_sha(ref: str) -> str:
    result = subprocess.run(
        ["git", "rev-parse", "--verify", f"{ref}^{{commit}}"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise ValueError(f"Could not resolve git ref '{ref}': {detail or 'unknown ref'}")
    return result.stdout.strip()


def short_sha(sha: str) -> str:
    return sha[:12] if sha else "?"


def normalize_git_remote_for_http(remote_url: str | None) -> str | None:
    value = (remote_url or "").strip()
    if not value:
        return None
    if value.startswith("git@github.com:"):
        repo_path = value[len("git@github.com:"):].rstrip("/")
        if repo_path.endswith(".git"):
            repo_path = repo_path[:-4]
        return f"https://github.com/{repo_path}"
    if value.startswith("https://github.com/") or value.startswith("http://github.com/"):
        repo_path = value.split("github.com/", 1)[1].rstrip("/")
        if repo_path.endswith(".git"):
            repo_path = repo_path[:-4]
        return f"https://github.com/{repo_path}"
    return None


def normalize_git_remote_for_clone(remote_url: str | None) -> str | None:
    value = (remote_url or "").strip()
    if not value:
        return None
    if value.startswith("git@github.com:"):
        repo_path = value[len("git@github.com:"):].rstrip("/")
        if repo_path.endswith(".git"):
            return f"https://github.com/{repo_path}"
        return f"https://github.com/{repo_path}.git"
    if value.startswith("https://github.com/") or value.startswith("http://github.com/"):
        repo_path = value.split("github.com/", 1)[1].rstrip("/")
        if repo_path.endswith(".git"):
            return f"https://github.com/{repo_path}"
        return f"https://github.com/{repo_path}.git"
    return None


def git_origin_url(
    repo_root: Path = ROOT,
    *,
    run_fn=None,
) -> str | None:
    run_fn = run_fn or subprocess.run
    run = run_fn(
        ["git", "remote", "get-url", "origin"],
        cwd=repo_root,
        capture_output=True,
        text=True,
        check=False,
    )
    if run.returncode != 0:
        return None
    return run.stdout.strip()


def git_origin_http_url(
    repo_root: Path = ROOT,
    *,
    run_fn=None,
) -> str | None:
    return normalize_git_remote_for_http(git_origin_url(repo_root, run_fn=run_fn))


def git_origin_clone_url(
    repo_root: Path = ROOT,
    *,
    run_fn=None,
) -> str | None:
    return normalize_git_remote_for_clone(git_origin_url(repo_root, run_fn=run_fn))


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
