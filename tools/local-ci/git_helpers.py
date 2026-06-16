"""Compatibility re-exports for local-CI git/time helpers.

Keep this module import-compatible while focused helpers own time,
git-ref, remote URL, and subprocess behavior.
"""

from __future__ import annotations

from git_command_helpers import run_git
from git_ref_helpers import (
    ROOT,
    current_branch,
    current_sha,
    git_root_for,
    resolve_git_ref_sha,
    short_sha,
)
from git_remote_helpers import (
    git_origin_clone_url,
    git_origin_http_url,
    git_origin_url,
    normalize_git_remote_for_clone,
    normalize_git_remote_for_http,
)
from git_time_helpers import now_iso

__all__ = [
    "ROOT",
    "current_branch",
    "current_sha",
    "git_origin_clone_url",
    "git_origin_http_url",
    "git_origin_url",
    "git_root_for",
    "normalize_git_remote_for_clone",
    "normalize_git_remote_for_http",
    "now_iso",
    "resolve_git_ref_sha",
    "run_git",
    "short_sha",
]
