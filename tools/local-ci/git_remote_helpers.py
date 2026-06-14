"""Git remote URL helpers for local CI."""

from __future__ import annotations

import subprocess
from pathlib import Path

from git_ref_helpers import ROOT


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
